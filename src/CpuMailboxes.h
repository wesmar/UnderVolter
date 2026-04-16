// CpuMailboxes.h — Generic CPU mailbox abstraction: MailboxBody union, per-mailbox
//                  configuration, and the unified CpuMailbox_ReadWrite dispatcher.
#pragma once

/*******************************************************************************
 * Mailbox transport type selector
 ******************************************************************************/

enum MailboxType
{
  MAILBOX_MSR =   0x00,             // Mailbox uses MSR for I/O
  MAILBOX_MMIO =  0x01,
};

/*******************************************************************************
 * CpuMailbox - Generic abstraction for several x64 mailboxes
 ******************************************************************************/

typedef union _MailboxBody
{
  UINT64 u64;
  UINT8  u8[8];

  struct {
    UINT32 data;
    UINT32 ifce;
  } box;

  struct {
    UINT32 lo;
    UINT32 hi;
  } u32;

  struct {
    UINT8 b0;
    UINT8 b1;
    UINT8 b2;
    UINT8 b3;
    UINT8 b4;
    UINT8 b5;
    UINT8 b6;
    UINT8 b7;
  } bytes;

} MailboxBody;

/*******************************************************************************
 * MailboxCfg
 ******************************************************************************/

typedef struct _MailboxCfg
{
  UINT32  addr;                           // MSR Index or MMIO addr
  UINT32  latency;                        // Typical write latency (in ns)
  UINT32  cmdBits;                        // Bit mask of the command
  UINT32  maxSpins;                       // Maximum probes while busy-waiting
  UINT32  busyFlag;                       // Busy Flag to test against
  UINT32  statusBits;                     // Bit mask of the return status
  UINT32  maxRetries;                     // Max. retries before aborting

  UINT8   type;                           // Mailbox type: MSR or MMIO
  UINT8   pad[3];
} MailboxCfg;

/*******************************************************************************
 * CpuMailbox - Generic abstraction for several mailboxes
 ******************************************************************************/

typedef struct _CpuMailbox
{
  MailboxBody b;
  MailboxCfg  cfg;
  UINT32 status;
  UINT8 pad[4];
} CpuMailbox;

/*******************************************************************************
 * CpuMailbox_ReadWrite
 ******************************************************************************/

EFI_STATUS EFIAPI CpuMailbox_ReadWrite(CpuMailbox* b);
