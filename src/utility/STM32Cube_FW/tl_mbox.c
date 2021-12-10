/**
 ******************************************************************************
 * @file    tl_mbox.c
 * @author  MCD Application Team
 * @brief   Transport layer for the mailbox interface
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2018-2021 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */

#if defined(STM32WBxx)
/* Includes ------------------------------------------------------------------*/
#include "stm32_wpan_common.h"
#include "hw.h"

#include "stm_list.h"
#include "tl.h"
#include "mbox_def.h"

/* Private typedef -----------------------------------------------------------*/
typedef enum
{
  TL_MB_MM_RELEASE_BUFFER,
  TL_MB_BLE_CMD,
  TL_MB_BLE_CMD_RSP,
  TL_MB_BLE_ASYNCH_EVT,
  TL_MB_SYS_CMD,
  TL_MB_SYS_CMD_RSP,
  TL_MB_SYS_ASYNCH_EVT,
} TL_MB_PacketType_t;

/* Private defines -----------------------------------------------------------*/
/* Private macros ------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/

/**< reference table */
PLACE_IN_SECTION("MAPPING_TABLE") static volatile MB_RefTable_t TL_RefTable;
PLACE_IN_SECTION("MB_MEM1") ALIGN(4) static MB_DeviceInfoTable_t TL_DeviceInfoTable;
PLACE_IN_SECTION("MB_MEM1") ALIGN(4) static MB_BleTable_t TL_BleTable;
PLACE_IN_SECTION("MB_MEM1") ALIGN(4) static MB_ThreadTable_t TL_ThreadTable;
PLACE_IN_SECTION("MB_MEM1") ALIGN(4) static MB_LldTestsTable_t TL_LldTestsTable;
PLACE_IN_SECTION("MB_MEM1") ALIGN(4) static MB_BleLldTable_t TL_BleLldTable;
PLACE_IN_SECTION("MB_MEM1") ALIGN(4) static MB_SysTable_t TL_SysTable;
PLACE_IN_SECTION("MB_MEM1") ALIGN(4) static MB_MemManagerTable_t TL_MemManagerTable;
PLACE_IN_SECTION("MB_MEM1") ALIGN(4) static MB_TracesTable_t TL_TracesTable;

/**< tables */
PLACE_IN_SECTION("MB_MEM1") ALIGN(4) static tListNode  FreeBufQueue;
PLACE_IN_SECTION("MB_MEM1") ALIGN(4) static tListNode  TracesEvtQueue;
PLACE_IN_SECTION("MB_MEM2") ALIGN(4) static uint8_t    CsBuffer[sizeof(TL_PacketHeader_t) + TL_EVT_HDR_SIZE + sizeof(TL_CsEvt_t)];
PLACE_IN_SECTION("MB_MEM2") ALIGN(4) static tListNode  EvtQueue;
PLACE_IN_SECTION("MB_MEM2") ALIGN(4) static tListNode  SystemEvtQueue;


static tListNode  LocalFreeBufQueue;
static void (* BLE_IoBusEvtCallBackFunction) (TL_EvtPacket_t *phcievt);
static void (* BLE_IoBusAclDataTxAck) ( void );
static void (* SYS_CMD_IoBusCallBackFunction) (TL_EvtPacket_t *phcievt);
static void (* SYS_EVT_IoBusCallBackFunction) (TL_EvtPacket_t *phcievt);


/* Global variables ----------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
static void SendFreeBuf( void );
static void OutputDbgTrace(TL_MB_PacketType_t packet_type, uint8_t* buffer);

/* Public Functions Definition ------------------------------------------------------*/

/******************************************************************************
 * GENERAL
 ******************************************************************************/
void TL_Enable( void )
{
  HW_IPCC_Enable();

  return;
}


void TL_Init( void )
{
  TL_RefTable.p_device_info_table = &TL_DeviceInfoTable;
  TL_RefTable.p_ble_table = &TL_BleTable;
  TL_RefTable.p_thread_table = &TL_ThreadTable;
  TL_RefTable.p_lld_tests_table = &TL_LldTestsTable;
  TL_RefTable.p_ble_lld_table = &TL_BleLldTable;
  TL_RefTable.p_sys_table = &TL_SysTable;
  TL_RefTable.p_mem_manager_table = &TL_MemManagerTable;
  TL_RefTable.p_traces_table = &TL_TracesTable;
  HW_IPCC_Init();

  return;
}

/******************************************************************************
 * BLE
 ******************************************************************************/
int32_t TL_BLE_Init( void* pConf )
{
  MB_BleTable_t  * p_bletable;

  TL_BLE_InitConf_t *pInitHciConf = (TL_BLE_InitConf_t *) pConf;

  LST_init_head (&EvtQueue);

  p_bletable = TL_RefTable.p_ble_table;

  p_bletable->pcmd_buffer = pInitHciConf->p_cmdbuffer;
  p_bletable->phci_acl_data_buffer = pInitHciConf->p_AclDataBuffer;
  p_bletable->pcs_buffer  = (uint8_t*)CsBuffer;
  p_bletable->pevt_queue  = (uint8_t*)&EvtQueue;

  HW_IPCC_BLE_Init();

  BLE_IoBusEvtCallBackFunction = pInitHciConf->IoBusEvtCallBack;
  BLE_IoBusAclDataTxAck = pInitHciConf->IoBusAclDataTxAck;

  return 0;
}

int32_t TL_BLE_SendCmd( uint8_t* buffer, uint16_t size )
{
  (void)(buffer);
  (void)(size);

  ((TL_CmdPacket_t*)(TL_RefTable.p_ble_table->pcmd_buffer))->cmdserial.type = TL_BLECMD_PKT_TYPE;

  OutputDbgTrace(TL_MB_BLE_CMD, TL_RefTable.p_ble_table->pcmd_buffer);

  HW_IPCC_BLE_SendCmd();

  return 0;
}

void HW_IPCC_BLE_RxEvtNot(void)
{
  TL_EvtPacket_t *phcievt;

  while(LST_is_empty(&EvtQueue) == FALSE)
  {
    LST_remove_head (&EvtQueue, (tListNode **)&phcievt);

    if ( ((phcievt->evtserial.evt.evtcode) == TL_BLEEVT_CS_OPCODE) || ((phcievt->evtserial.evt.evtcode) == TL_BLEEVT_CC_OPCODE ) )
    {
      OutputDbgTrace(TL_MB_BLE_CMD_RSP, (uint8_t*)phcievt);
    }
    else
    {
      OutputDbgTrace(TL_MB_BLE_ASYNCH_EVT, (uint8_t*)phcievt);
    }

    BLE_IoBusEvtCallBackFunction(phcievt);
  }

  return;
}

int32_t TL_BLE_SendAclData( uint8_t* buffer, uint16_t size )
{
  (void)(buffer);
  (void)(size);

  ((TL_AclDataPacket_t *)(TL_RefTable.p_ble_table->phci_acl_data_buffer))->AclDataSerial.type = TL_ACL_DATA_PKT_TYPE;

  HW_IPCC_BLE_SendAclData();

  return 0;
}

void HW_IPCC_BLE_AclDataAckNot(void)
{
  BLE_IoBusAclDataTxAck( );

  return;
}

/******************************************************************************
 * SYSTEM
 ******************************************************************************/
int32_t TL_SYS_Init( void* pConf  )
{
  MB_SysTable_t  * p_systable;

  TL_SYS_InitConf_t *pInitHciConf = (TL_SYS_InitConf_t *) pConf;

  LST_init_head (&SystemEvtQueue);
  p_systable = TL_RefTable.p_sys_table;
  p_systable->pcmd_buffer = pInitHciConf->p_cmdbuffer;
  p_systable->sys_queue = (uint8_t*)&SystemEvtQueue;

  HW_IPCC_SYS_Init();

  SYS_CMD_IoBusCallBackFunction = pInitHciConf->IoBusCallBackCmdEvt;
  SYS_EVT_IoBusCallBackFunction = pInitHciConf->IoBusCallBackUserEvt;

  return 0;
}

int32_t TL_SYS_SendCmd( uint8_t* buffer, uint16_t size )
{
  (void)(buffer);
  (void)(size);

  ((TL_CmdPacket_t *)(TL_RefTable.p_sys_table->pcmd_buffer))->cmdserial.type = TL_SYSCMD_PKT_TYPE;

  OutputDbgTrace(TL_MB_SYS_CMD, TL_RefTable.p_sys_table->pcmd_buffer);

  HW_IPCC_SYS_SendCmd();

  return 0;
}

void HW_IPCC_SYS_CmdEvtNot(void)
{
  OutputDbgTrace(TL_MB_SYS_CMD_RSP, (uint8_t*)(TL_RefTable.p_sys_table->pcmd_buffer) );

  SYS_CMD_IoBusCallBackFunction( (TL_EvtPacket_t*)(TL_RefTable.p_sys_table->pcmd_buffer) );

  return;
}

void HW_IPCC_SYS_EvtNot( void )
{
  TL_EvtPacket_t *p_evt;

  while(LST_is_empty(&SystemEvtQueue) == FALSE)
  {
    LST_remove_head (&SystemEvtQueue, (tListNode **)&p_evt);

    OutputDbgTrace(TL_MB_SYS_ASYNCH_EVT, (uint8_t*)p_evt );

    SYS_EVT_IoBusCallBackFunction( p_evt );
  }

  return;
}

/******************************************************************************
 * THREAD
 ******************************************************************************/
#ifdef THREAD_WB
void TL_THREAD_Init( TL_TH_Config_t *p_Config )
{
  MB_ThreadTable_t  * p_thread_table;

  p_thread_table = TL_RefTable.p_thread_table;

  p_thread_table->clicmdrsp_buffer = p_Config->p_ThreadCliRspBuffer;
  p_thread_table->otcmdrsp_buffer = p_Config->p_ThreadOtCmdRspBuffer;
  p_thread_table->notack_buffer = p_Config->p_ThreadNotAckBuffer;

  HW_IPCC_THREAD_Init();

  return;
}

void TL_OT_SendCmd( void )
{
  ((TL_CmdPacket_t *)(TL_RefTable.p_thread_table->otcmdrsp_buffer))->cmdserial.type = TL_OTCMD_PKT_TYPE;

  HW_IPCC_OT_SendCmd();

  return;
}

void TL_CLI_SendCmd( void )
{
  ((TL_CmdPacket_t *)(TL_RefTable.p_thread_table->clicmdrsp_buffer))->cmdserial.type = TL_CLICMD_PKT_TYPE;

  HW_IPCC_CLI_SendCmd();

  return;
}

void TL_THREAD_SendAck ( void )
{
  ((TL_CmdPacket_t *)(TL_RefTable.p_thread_table->notack_buffer))->cmdserial.type = TL_OTACK_PKT_TYPE;

  HW_IPCC_THREAD_SendAck();

  return;
}

void TL_THREAD_CliSendAck ( void )
{
  ((TL_CmdPacket_t *)(TL_RefTable.p_thread_table->notack_buffer))->cmdserial.type = TL_OTACK_PKT_TYPE;

  HW_IPCC_THREAD_CliSendAck();

  return;
}

void HW_IPCC_OT_CmdEvtNot(void)
{
  TL_OT_CmdEvtReceived( (TL_EvtPacket_t*)(TL_RefTable.p_thread_table->otcmdrsp_buffer) );

  return;
}

void HW_IPCC_THREAD_EvtNot( void )
{
  TL_THREAD_NotReceived( (TL_EvtPacket_t*)(TL_RefTable.p_thread_table->notack_buffer) );

  return;
}

void HW_IPCC_THREAD_CliEvtNot( void )
{
  TL_THREAD_CliNotReceived( (TL_EvtPacket_t*)(TL_RefTable.p_thread_table->clicmdrsp_buffer) );

  return;
}

__WEAK void TL_OT_CmdEvtReceived( TL_EvtPacket_t * Otbuffer  ){};
__WEAK void TL_THREAD_NotReceived( TL_EvtPacket_t * Notbuffer ){};
__WEAK void TL_THREAD_CliNotReceived( TL_EvtPacket_t * Notbuffer ){};

#endif /* THREAD_WB */

/******************************************************************************
 * LLD TESTS
 ******************************************************************************/
#ifdef LLD_TESTS_WB
void TL_LLDTESTS_Init( TL_LLD_tests_Config_t *p_Config )
{
  MB_LldTestsTable_t  * p_lld_tests_table;

  p_lld_tests_table = TL_RefTable.p_lld_tests_table;
  p_lld_tests_table->clicmdrsp_buffer = p_Config->p_LldTestsCliCmdRspBuffer;
  p_lld_tests_table->m0cmd_buffer = p_Config->p_LldTestsM0CmdBuffer;
  HW_IPCC_LLDTESTS_Init();
  return;
}

void TL_LLDTESTS_SendCliCmd( void )
{
  ((TL_CmdPacket_t *)(TL_RefTable.p_lld_tests_table->clicmdrsp_buffer))->cmdserial.type = TL_CLICMD_PKT_TYPE;
  HW_IPCC_LLDTESTS_SendCliCmd();
  return;
}

void HW_IPCC_LLDTESTS_ReceiveCliRsp( void )
{
  TL_LLDTESTS_ReceiveCliRsp( (TL_CmdPacket_t*)(TL_RefTable.p_lld_tests_table->clicmdrsp_buffer) );
  return;
}

void TL_LLDTESTS_SendCliRspAck( void )
{
  HW_IPCC_LLDTESTS_SendCliRspAck();
  return;
}

void HW_IPCC_LLDTESTS_ReceiveM0Cmd( void )
{
  TL_LLDTESTS_ReceiveM0Cmd( (TL_CmdPacket_t*)(TL_RefTable.p_lld_tests_table->m0cmd_buffer) );
  return;
}


void TL_LLDTESTS_SendM0CmdAck( void )
{
  HW_IPCC_LLDTESTS_SendM0CmdAck();
  return;
}

__WEAK void TL_LLDTESTS_ReceiveCliRsp( TL_CmdPacket_t * Notbuffer ){};
__WEAK void TL_LLDTESTS_ReceiveM0Cmd( TL_CmdPacket_t * Notbuffer ){};
#endif /* LLD_TESTS_WB */

/******************************************************************************
 * BLE LLD
 ******************************************************************************/
#ifdef BLE_LLD_WB
void TL_BLE_LLD_Init( TL_BLE_LLD_Config_t *p_Config )
{
  MB_BleLldTable_t  * p_ble_lld_table;

  p_ble_lld_table = TL_RefTable.p_ble_lld_table;
  p_ble_lld_table->cmdrsp_buffer = p_Config->p_BleLldCmdRspBuffer;
  p_ble_lld_table->m0cmd_buffer = p_Config->p_BleLldM0CmdBuffer;
  HW_IPCC_BLE_LLD_Init();
  return;
}

void TL_BLE_LLD_SendCliCmd( void )
{
  ((TL_CmdPacket_t *)(TL_RefTable.p_ble_lld_table->cmdrsp_buffer))->cmdserial.type = TL_CLICMD_PKT_TYPE;
  HW_IPCC_BLE_LLD_SendCliCmd();
  return;
}

void HW_IPCC_BLE_LLD_ReceiveCliRsp( void )
{
  TL_BLE_LLD_ReceiveCliRsp( (TL_CmdPacket_t*)(TL_RefTable.p_ble_lld_table->cmdrsp_buffer) );
  return;
}

void TL_BLE_LLD_SendCliRspAck( void )
{
  HW_IPCC_BLE_LLD_SendCliRspAck();
  return;
}

void HW_IPCC_BLE_LLD_ReceiveM0Cmd( void )
{
  TL_BLE_LLD_ReceiveM0Cmd( (TL_CmdPacket_t*)(TL_RefTable.p_ble_lld_table->m0cmd_buffer) );
  return;
}


void TL_BLE_LLD_SendM0CmdAck( void )
{
  HW_IPCC_BLE_LLD_SendM0CmdAck();
  return;
}

__WEAK void TL_BLE_LLD_ReceiveCliRsp( TL_CmdPacket_t * Notbuffer ){};
__WEAK void TL_BLE_LLD_ReceiveM0Cmd( TL_CmdPacket_t * Notbuffer ){};

/* Transparent Mode */
void TL_BLE_LLD_SendCmd( void )
{
  ((TL_CmdPacket_t *)(TL_RefTable.p_ble_lld_table->cmdrsp_buffer))->cmdserial.type = TL_CLICMD_PKT_TYPE;
  HW_IPCC_BLE_LLD_SendCmd();
  return;
}

void HW_IPCC_BLE_LLD_ReceiveRsp( void )
{
  TL_BLE_LLD_ReceiveRsp( (TL_CmdPacket_t*)(TL_RefTable.p_ble_lld_table->cmdrsp_buffer) );
  return;
}

void TL_BLE_LLD_SendRspAck( void )
{
  HW_IPCC_BLE_LLD_SendRspAck();
  return;
}
#endif /* BLE_LLD_WB */

/******************************************************************************
 * MEMORY MANAGER
 ******************************************************************************/
void TL_MM_Init( TL_MM_Config_t *p_Config )
{
  static MB_MemManagerTable_t  * p_mem_manager_table;

  LST_init_head (&FreeBufQueue);
  LST_init_head (&LocalFreeBufQueue);

  p_mem_manager_table = TL_RefTable.p_mem_manager_table;

  p_mem_manager_table->blepool = p_Config->p_AsynchEvtPool;
  p_mem_manager_table->blepoolsize = p_Config->AsynchEvtPoolSize;
  p_mem_manager_table->pevt_free_buffer_queue = (uint8_t*)&FreeBufQueue;
  p_mem_manager_table->spare_ble_buffer = p_Config->p_BleSpareEvtBuffer;
  p_mem_manager_table->spare_sys_buffer = p_Config->p_SystemSpareEvtBuffer;
  p_mem_manager_table->traces_evt_pool = p_Config->p_TracesEvtPool;
  p_mem_manager_table->tracespoolsize = p_Config->TracesEvtPoolSize;

  return;
}

void TL_MM_EvtDone(TL_EvtPacket_t * phcievt)
{
  LST_insert_tail(&LocalFreeBufQueue, (tListNode *)phcievt);

  OutputDbgTrace(TL_MB_MM_RELEASE_BUFFER, (uint8_t*)phcievt);

  HW_IPCC_MM_SendFreeBuf( SendFreeBuf );

  return;
}

static void SendFreeBuf( void )
{
  tListNode *p_node;

  while ( FALSE == LST_is_empty (&LocalFreeBufQueue) )
  {
    LST_remove_head( &LocalFreeBufQueue, (tListNode **)&p_node );
    LST_insert_tail( (tListNode*)(TL_RefTable.p_mem_manager_table->pevt_free_buffer_queue), p_node );
  }

  return;
}

/******************************************************************************
 * TRACES
 ******************************************************************************/
void TL_TRACES_Init( void )
{
  LST_init_head (&TracesEvtQueue);

  TL_RefTable.p_traces_table->traces_queue = (uint8_t*)&TracesEvtQueue;

  HW_IPCC_TRACES_Init();

  return;
}

void HW_IPCC_TRACES_EvtNot(void)
{
  TL_EvtPacket_t *phcievt;

  while(LST_is_empty(&TracesEvtQueue) == FALSE)
  {
    LST_remove_head (&TracesEvtQueue, (tListNode **)&phcievt);
    TL_TRACES_EvtReceived( phcievt );
  }

  return;
}

__WEAK void TL_TRACES_EvtReceived( TL_EvtPacket_t * hcievt )
{
  (void)(hcievt);
}

/******************************************************************************
 * DEBUG INFORMATION
 ******************************************************************************/
static void OutputDbgTrace(TL_MB_PacketType_t packet_type, uint8_t* buffer)
{
  /* Function stubbed */
  UNUSED(packet_type);
  UNUSED(buffer);

  return;
}

#endif /* STM32WBxx */
