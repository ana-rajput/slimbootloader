/** @file

Copyright (c) 2022, Intel Corporation. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Library/TcoTimerLib.h>
#include <Library/BootloaderCommonLib.h>
#include <Library/ResetSystemLib.h>
#include <Library/DebugLib.h>
#include <Library/FirmwareResiliencyLib.h>
#include <Library/TopSwapLib.h>
#include <Library/PcdLib.h>
#include <Library/WatchDogTimerLib.h>
#include <Library/SpiFlashLib.h>
#include <Library/VariableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BootloaderCoreLib.h>
#include <Pi/PiBootMode.h>
#include <FirmwareUpdateStatus.h>
#include <RecoveryStatus.h>

STATIC CONST CHAR16 *mRecoveryStatusVariableName = RECOVERY_STATUS_VARIABLE_NAME;

/**
  Retrieve FW update state from the reserved region

  @retval StateMachine The current FWU state machine
**/
UINT8
EFIAPI
GetFwuStateMachine (
  VOID
  )
{
  FW_UPDATE_STATUS    FwUpdStatus;
  EFI_STATUS          Status;
  UINT32              RsvdBase;
  UINT32              RsvdSize;

  Status = GetComponentInfoByPartition (FLASH_MAP_SIG_BLRESERVED, FALSE, &RsvdBase, &RsvdSize);
  if (!EFI_ERROR (Status) && RsvdBase >= PcdGet32(PcdFlashBaseAddress)) {
    RsvdBase -= PcdGet32(PcdFlashBaseAddress);
    Status = SpiFlashRead (FlashRegionBios, RsvdBase, sizeof (FwUpdStatus), (UINT8 *)&FwUpdStatus);
    if (!EFI_ERROR (Status) &&
        ((FwUpdStatus.Signature == FW_UPDATE_STATUS_SIGNATURE) ||
         (FwUpdStatus.Signature == FW_RECOVERY_STATUS_SIGNATURE))) {
      return FwUpdStatus.StateMachine;
    }
  }
  return FW_UPDATE_SM_INIT;
}

/**
  Initialize a RECOVERY_STATUS structure to its default (no recovery) state.

  @param[out] Status  The structure to initialize.
**/
STATIC
VOID
InitDefaultRecoveryStatus (
  OUT RECOVERY_STATUS  *Status
  )
{
  ZeroMem (Status, sizeof (RECOVERY_STATUS));
  Status->Revision        = RECOVERY_STATUS_REVISION;
  Status->Reason          = RECOVERY_REASON_NONE;
  Status->AttemptCount    = 0;
  Status->LastResult      = RECOVERY_RESULT_PENDING;
  Status->FailedBootCount = 0;
}

/**
  Persist the RECOVERY_STATUS variable to the SBL variable region.

  @param[in] Status  The structure to write.

  @retval EFI_SUCCESS  The variable was written successfully.
  @retval others       The write failed.
**/
STATIC
EFI_STATUS
SaveRecoveryStatus (
  IN RECOVERY_STATUS  *Status
  )
{
  return SetVariable (
           (CHAR16 *)mRecoveryStatusVariableName,
           &gRecoveryStatusVariableGuid,
           EFI_VARIABLE_NON_VOLATILE,
           sizeof (RECOVERY_STATUS),
           Status
           );
}

/**
  Delete the RECOVERY_STATUS variable from the SBL variable region.
**/
STATIC
VOID
DeleteRecoveryStatus (
  VOID
  )
{
  //
  // A zero DataSize deletes the variable in LiteVariableLib.
  //
  SetVariable (
    (CHAR16 *)mRecoveryStatusVariableName,
    &gRecoveryStatusVariableGuid,
    EFI_VARIABLE_NON_VOLATILE,
    0,
    NULL
    );
}

/**
  Detect whether ACM reported corruption in the IBB.

  @retval TRUE   An SBL (ACM-detected) failure was found.
  @retval FALSE  No ACM failure detected.
**/
STATIC
BOOLEAN
DetectAcmFailure (
  VOID
  )
{
  UINT8 StateMachine;

  // If already marked in recovery path, no need to re-detect.
  if (IsRecoveryTriggered ()) {
    return FALSE;
  }

  StateMachine = GetFwuStateMachine ();
  switch (StateMachine) {
    case FW_UPDATE_SM_PART_A:
      if (GetCurrentBootPartition () == PrimaryPartition) {
        DEBUG ((DEBUG_INFO, "Partition to be updated is same as current boot partition (primary)\n"));
        return TRUE;
      }
      break;

    case FW_UPDATE_SM_PART_B:
      if (GetCurrentBootPartition () == BackupPartition) {
        DEBUG ((DEBUG_INFO, "Partition to be updated is same as current boot partition (backup)\n"));
        return TRUE;
      }
      break;

    default:
      if (GetCurrentBootPartition () == BackupPartition) {
        DEBUG ((DEBUG_INFO, "Booting from backup partition outside of update\n"));
        return TRUE;
      }
      break;
  }

  return FALSE;
}

/**
  Unified resiliency check point.

  Consolidates ACM and TCO checks and maintains persistent recovery state.
  Must be called after BoardInit(PostConfigInit) and before FSP-M.

  @param[in] BootFailureThreshold  Consecutive TCO timeouts before recovery.
  @param[in] MaxRecoveryAttempts   Recovery attempts before CpuHalt().
**/
VOID
EFIAPI
UnifiedResiliencyCheck (
  IN UINT8  BootFailureThreshold,
  IN UINT8  MaxRecoveryAttempts
  )
{
  RECOVERY_STATUS   Status;
  UINTN             Size;
  EFI_STATUS        EfiStatus;
  BOOLEAN           VarExists;
  BOOLEAN           NeedPartitionSwitch;
  UINT8             NewReason;
  BOOT_PARTITION    NewPartition;
  // Skip explicit capsule-update boots; still run for recovery-triggered boots.
  if ((GetBootMode () == BOOT_ON_FLASH_UPDATE) && !IsRecoveryTriggered ()) {
    return;
  }
  //
  // Read existing "RecoveryStatus" variable (may not exist).
  //
  ZeroMem (&Status, sizeof (Status));
  Size      = sizeof (RECOVERY_STATUS);
  EfiStatus = GetVariable (
                (CHAR16 *)mRecoveryStatusVariableName,
                &gRecoveryStatusVariableGuid,
                NULL,
                &Size,
                &Status
                );

  // Handle old RecoveryStatus size from prior builds.
  if (!EFI_ERROR (EfiStatus) && (Size != sizeof (RECOVERY_STATUS))) {
    if ((Status.Reason != RECOVERY_REASON_NONE) || (Status.LastResult == RECOVERY_RESULT_PENDING)) {
      DEBUG ((DEBUG_INFO, "Resiliency: detected old format with active recovery (size %u), migrating\n", (UINTN)Size));
      Status.Revision = RECOVERY_STATUS_REVISION;
      Status.Reserved = 0;
      EfiStatus = SaveRecoveryStatus (&Status);
      if (EFI_ERROR (EfiStatus)) {
        CpuHalt ("Resiliency: failed to migrate RecoveryStatus to new format\n");
      }
      VarExists = TRUE;
    } else {
      DEBUG ((DEBUG_INFO, "Resiliency: detected stale old format RecoveryStatus (size %u), deleting\n", (UINTN)Size));
      DeleteRecoveryStatus ();
      VarExists = FALSE;
    }
  } else {
    VarExists = (!EFI_ERROR (EfiStatus) && (Size == sizeof (RECOVERY_STATUS)));
    if (VarExists && (Status.Revision != RECOVERY_STATUS_REVISION)) {
      DEBUG ((DEBUG_WARN, "Resiliency: invalid RecoveryStatus revision %u, deleting\n", Status.Revision));
      DeleteRecoveryStatus ();
      VarExists = FALSE;
    }
  }

  // If variable exists, handle post-recovery and stale-counter cleanup.
  if (VarExists) {
    // Clear state after successful recovery.
    if (Status.LastResult == RECOVERY_RESULT_SUCCESS) {
      DEBUG ((DEBUG_INFO, "Resiliency: recovery succeeded, clearing state\n"));
      DeleteRecoveryStatus ();
      ClearRecoveryTrigger ();
      VarExists = FALSE;
    }

    // Drop stale non-consecutive TCO count.
    if ((Status.Reason == RECOVERY_REASON_NONE) &&
        (Status.FailedBootCount > 0) &&
        !WasBootCausedByTcoTimeout ()) {
      DEBUG ((DEBUG_INFO, "Resiliency: clearing stale TCO boot counter\n"));
      DeleteRecoveryStatus ();
      VarExists = FALSE;
    }
  }

  //
  // Detect failures.
  //
  NewReason           = RECOVERY_REASON_NONE;
  NeedPartitionSwitch = FALSE;

  //
  // ACM failure detection.
  //
  if (DetectAcmFailure ()) {
    NewReason           |= RECOVERY_REASON_SBL;
  }

  //
  // TCO timer failure detection.
  //
  if (WasBootCausedByTcoTimeout ()) {
    ClearTcoStatus ();

    if (!VarExists) {
      InitDefaultRecoveryStatus (&Status);
      VarExists = TRUE;
    }

    Status.FailedBootCount++;
    DEBUG ((DEBUG_INFO, "Boot failure occurred! Failed boot count: %d\n", Status.FailedBootCount));

    if (Status.FailedBootCount >= BootFailureThreshold) {
      NewReason          |= RECOVERY_REASON_SBL;
      NeedPartitionSwitch = TRUE;
    } else {
      // Below threshold: persist TCO counter.
      EfiStatus = SaveRecoveryStatus (&Status);
      if (EFI_ERROR (EfiStatus)) {
        CpuHalt ("Resiliency: failed to persist TCO counter state\n");
      }
      if (Status.Reason == RECOVERY_REASON_NONE) {
        return;
      }
    }
  }

  //
  // No new failure detected this boot.
  //
  if (NewReason == RECOVERY_REASON_NONE) {
    if (VarExists && (Status.Reason != RECOVERY_REASON_NONE)) {
      // Recovery already pending from a prior boot.
      if (Status.LastResult != RECOVERY_RESULT_PENDING) {
        // Previous attempt failed; count retry.
        Status.AttemptCount++;
        Status.LastResult = RECOVERY_RESULT_PENDING;
        if (Status.AttemptCount > MaxRecoveryAttempts) {
          CpuHalt ("Recovery failed after maximum attempts\n");
        }
        EfiStatus = SaveRecoveryStatus (&Status);
        if (EFI_ERROR (EfiStatus)) {
          CpuHalt ("Resiliency: failed to persist retry state\n");
        }
      }
      //
      // Keep trigger set so FW update payload runs recovery.
      //
      SetRecoveryTrigger ();
    }
    //
    // Otherwise no recovery is needed - continue normal boot.
    //
    return;
  }

  //
  // Recovery needed - update or create the variable.
  //
  if (VarExists) {
    Status.Reason     |= NewReason;
    Status.AttemptCount += 1;
    Status.LastResult   = RECOVERY_RESULT_PENDING;
  } else {
    InitDefaultRecoveryStatus (&Status);
    Status.Reason       = NewReason;
    Status.AttemptCount = 1;
    Status.LastResult   = RECOVERY_RESULT_PENDING;
  }

  //
  // Anti-loop protection.
  //
  if (Status.AttemptCount > MaxRecoveryAttempts) {
    CpuHalt ("Recovery failed after maximum attempts\n");
  }

  //
  // Write updated variable.
  //
  EfiStatus = SaveRecoveryStatus (&Status);
  if (EFI_ERROR (EfiStatus)) {
    CpuHalt ("Resiliency: failed to persist recovery state\n");
  }

  //
  // Ensure the WDT recovery trigger (BIT20) is set.
  //
  SetRecoveryTrigger ();

  SetBootMode (BOOT_ON_FLASH_UPDATE);
  //
  // Switch partition and cold reset when required.
  //
  if ((Status.Reason & RECOVERY_REASON_SBL) && NeedPartitionSwitch) {
    NewPartition = (GetCurrentBootPartition () == PrimaryPartition) ? BackupPartition : PrimaryPartition;
    DEBUG ((DEBUG_INFO, "Boot failure threshold reached! Switching to partition: %d\n", NewPartition));
    EfiStatus = SetBootPartition (NewPartition);
    if (EFI_ERROR (EfiStatus)) {
      CpuHalt ("Unable to recover partition, failed to switch boot partition!\n");
    }
    ResetSystem (EfiResetCold);
  }
}
