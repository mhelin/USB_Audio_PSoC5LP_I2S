/*******************************************************************************
* File Name: main.c
*
* Version: 2.0
*
* Description:
*  This USB example project implements a USB Audio Device that connects to the
*  PC via the USB interface. The USB Audio Device does not require a special
*  USB driver, because the USB Audio support is already built into Windows.
*  The device appears in the system as a mono speaker with 8-bit resolution and
*  a sample rate of 32 kHz.
*
********************************************************************************
* Copyright 2012-2015, Cypress Semiconductor Corporation. All rights reserved.
* This software is owned by Cypress Semiconductor Corporation and is protected
* by and subject to worldwide patent and copyright laws and treaties.
* Therefore, you may use this software only as provided in the license agreement
* accompanying the software package from which you obtained this software.
* CYPRESS AND ITS SUPPLIERS MAKE NO WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
* WITH REGARD TO THIS SOFTWARE, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT,
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
*******************************************************************************/

#include <project.h>
#include <stdio.h>

/* UBSFS device constants. */
#define USBFS_AUDIO_DEVICE  (0u)
#define AUDIO_INTERFACE     (1u)
#define OUT_EP_NUM          (2u)
#define AUDIO_CH            (2u)
#define BYTES_PER_CH        (2u)
#define USB_BUF_SIZE        (384u)

/* Audio buffer constants. */
#define TRANSFER_SIZE       (384u/AUDIO_CH/BYTES_PER_CH)
#define NUM_OF_BUFFERS      (10u)
#define BUFFER_SIZE         (TRANSFER_SIZE * NUM_OF_BUFFERS)
#define sHALF_BUFFER_SIZE   ((int16)(BUFFER_SIZE/2u))

#define I2S_DATA_SIZE       (I2S_DATA_BITS/8*AUDIO_CH)
#define I2S_TRANSFER_SIZE   (TRANSFER_SIZE*I2S_DATA_SIZE)
#define I2S_BUFFER_SIZE     (I2S_TRANSFER_SIZE * NUM_OF_BUFFERS)

/* Circular buffer for audio stream. */
uint8 tmpEpBuf[USB_BUF_SIZE];
uint8 soundBuffer_L[BUFFER_SIZE];
uint8 soundBuffer_R[BUFFER_SIZE];
uint8 soundBuffer_I2S[I2S_BUFFER_SIZE];
volatile uint16 outIndex = 0u;
volatile uint16 inIndex  = 0u;

/* Variables used to manage DMA. */
volatile uint8 syncDma = 0u;

/* Variable declarations for VDACoutDMA. */
uint8 VdacOutDmaCh_L;
uint8 VdacOutDmaCh_R;
uint8 VdacOutDmaTd_L[NUM_OF_BUFFERS];
uint8 VdacOutDmaTd_R[NUM_OF_BUFFERS];

/* DMA Configuration for VDACoutDMA (Memory to VDAC) */
#define VDAC_DMA_BYTES_PER_BURST    (1u)
#define VDAC_DMA_REQUEST_PER_BURST  (1u)
#define VDAC_DMA_TD_TERMOUT_EN      (VdacDma_L__TD_TERMOUT_EN)
#define VDAC_DMA_DST_BASE           (CYDEV_PERIPH_BASE)
#define VDAC_DMA_SRC_BASE           (CY_PSOC5LP) ? ((uint32) soundBuffer_L) : (CYDEV_SRAM_BASE)
#define VDAC_DMA_ENABLE_PRESERVE_TD (1u)

/* Variable declarations for I2S_DMA. */
uint8 I2SDmaCh;
uint8 I2SDmaTd[NUM_OF_BUFFERS];

/* DMA Configuration for I2S_DMA (Memory to I2S) */
#define I2S_DMA_BYTES_PER_BURST    (1u)
#define I2S_DMA_REQUEST_PER_BURST  (1u)
#define I2S_DMA_TD_TERMOUT_EN      (I2S_DMA__TD_TERMOUT_EN)
#define I2S_DMA_DST_BASE           (CYDEV_PERIPH_BASE)
#define I2S_DMA_SRC_BASE           (CY_PSOC5LP) ? ((uint32) soundBuffer_L) : (CYDEV_SRAM_BASE)
#define I2S_DMA_ENABLE_PRESERVE_TD (1u)

CY_ISR_PROTO(VdacDmaDone);

/* Configuration for internal sampling clock adjustment. */
#define adjustInterval              (10u)
#define IDACadjustInterval          (10)
#define adjustTic                   (80.0/100.0)
#define UpperAdjustRange            (-100)
#define LowerAdjustRange            (+100)
#define MovingAverageFactor         (0.1)
#define SGN(x)                      (((x)<0) ? -1 : (((x)>0) ? 1 : 0))

#define Vctrl_HIGH                  30000
#define Vctrl_LOW                   4000
#define Vctrl_HIGHEST               30000
#define Vctrl_LOWEST                4000

#define Vctrl_Read()                PWM_ReadCompare()
#define Vctrl_Write(x)              PWM_WriteCompare(x)
#define Vctrl_Start()               PWM_Start()
#define Vctrl_Stop()                PWM_Stop()

/* Debug print buffer. */
char dbuf[256];
#define dp(x)                       DP_PutString(x);

/* EZI2C buffer watched by external device (PC). */
struct _EZI2C_buf {
    uint16 clockDivider;
    uint16 dist;
    uint16 distAvrerage;
    int16 distAvDiff;
    float clockAdjust;
    uint8 L;
    uint8 R;
    uint8 flag;
    uint16 inIndex;
    uint16 outIndex;
    uint8 idac;
    uint16 vctrl;
} EZI2C_buf;

/*
* Operation Flag.
*  bit 0 => (unused)
*  bit 1 => DMA for VDAC is stopped due to buffer under-run.
*  bit 2 => USB packet is dropped due to buffer over-run.
*/
volatile uint8 flag = 0u;
#define DMA_STOP_FLAG            (1u<<1)
#define USB_DROP_FLAG            (1u<<2)

volatile uint16 dma_stop_count=0;
volatile uint16 usb_drop_count=0;
volatile int16 dma_usb_count=0;

void vctrl_set(uint16 v);

/*******************************************************************************
* Function Name: main
********************************************************************************
*
* Summary:
*
*  The main function performs the following actions:
*   1. Initializes LCD and VDAC components.
*   2. Configures the DMA channel and transfers the descriptors.
*      The source is souundBuffer (SRAM) and the destination is the VDAC data register.
*   3. Starts the USBFS component and waits until the device is got enumerated
*      by the host.
*   4. When the PC starts streaming, the DMA starts copying data into the VDAC with
*      the 32-kHz frequency. None of the synchronization methods is implemented:
*      If the PC (source) transfers data faster than the device (sink), transfers it  ????
*      to VDAC the extra samples are dropped.
*      If the PC (source) transfers data slower than the device (sink), transfers it  ????
*      to VDAC the transfer is stopped and starts after the buffer is half-full.
*
* Parameters:
*  None.
*
* Return:
*  None.
*
*******************************************************************************/

int main()
{
    uint8 i;
    uint16 j;
    uint8 td;
    uint16 readSize;
    uint8 skipNextOut = 0u;
    float fs;
    uint16 initialClockDivider = SampleClk_GetDividerRegister();
    uint16 clockDivider = initialClockDivider;
    float clockAdjust = 0;
    uint16 adjustIntervalCount=0u;
    uint16 IDACadjustIntervalCount=0u;
    uint16 dist;
    float distAverage;
    float lastDistAverage=0;
    float distAvDiff=0;

    uint8 initialIdac;
    uint8 idac;
    float idacAdj=0;
    uint16 initialVctrl;
    uint16 vctrl;
    float vctrlAdj=0;
    
    uint16 vctrlAdjMax;
    uint16 vctrlAdjMin;
    int8 lastSwap=0;

    /* Start UART for debug print. */
    DP_Start();
    dp("\n\n");
    dp("========================================\n");
    dp(" PSoC USB Audio start\n");
    dp("========================================\n");
                
    /* Start EZI2C for debug monitoring. */
    EZI2C_SetBuffer1(sizeof(EZI2C_buf), 0, (void *)&EZI2C_buf);
    EZI2C_Start();
    
    /* Start VDAC for clock generator. */
    Vctrl_Start();
    //Vctrl_Write((Vctrl_HIGH+Vctrl_LOW)/2);
    //Vctrl_Write(Vctrl_HIGH);
    Vctrl_Write(8600);
    vctrl=initialVctrl=Vctrl_Read();

    /* Start Comparator for clock generator. */
    CMP_Start();

    /* Start IDAC for clock generator. */
    IDAC_Start();
    //IDAC_SetValue(128);
    idac=initialIdac=IDAC_Data;

    /* Start VDAC8 to generate output wave. */
    VDAC8_L_Start();
    VDAC8_R_Start();

    /* Initialize DMA channel. */
    VdacOutDmaCh_L = VdacDma_L_DmaInitialize(VDAC_DMA_BYTES_PER_BURST, VDAC_DMA_REQUEST_PER_BURST,
                                            HI16(VDAC_DMA_SRC_BASE), HI16(VDAC_DMA_DST_BASE));
    VdacOutDmaCh_R = VdacDma_R_DmaInitialize(VDAC_DMA_BYTES_PER_BURST, VDAC_DMA_REQUEST_PER_BURST,
                                            HI16(VDAC_DMA_SRC_BASE), HI16(VDAC_DMA_DST_BASE));
    I2SDmaCh = I2S_DMA_DmaInitialize(I2S_DMA_BYTES_PER_BURST, I2S_DMA_REQUEST_PER_BURST,
                                            HI16(I2S_DMA_SRC_BASE), HI16(I2S_DMA_DST_BASE));

    /* Allocate transfer descriptors for each buffer chunk. */
    for (i = 0u; i < NUM_OF_BUFFERS; ++i)
    {
        VdacOutDmaTd_L[i] = CyDmaTdAllocate();
        VdacOutDmaTd_R[i] = CyDmaTdAllocate();
        I2SDmaTd[i] = CyDmaTdAllocate();
        sprintf(dbuf,"Alloc DMA TD[%d] Lch=%d Rch=%d I2S=%d\n",i,VdacOutDmaTd_L[i],VdacOutDmaTd_R[i],I2SDmaTd[i]);dp(dbuf);
    }

    /* Configure DMA transfer descriptors. */
    for (i = 0u; i < (NUM_OF_BUFFERS - 1u); ++i)
    {
        /* Chain current and next DMA transfer descriptors to be in row. */
        CyDmaTdSetConfiguration(VdacOutDmaTd_L[i], TRANSFER_SIZE, VdacOutDmaTd_L[i + 1u],
                                (TD_INC_SRC_ADR | VDAC_DMA_TD_TERMOUT_EN));
        CyDmaTdSetConfiguration(VdacOutDmaTd_R[i], TRANSFER_SIZE, VdacOutDmaTd_R[i + 1u],
                                (TD_INC_SRC_ADR));
        CyDmaTdSetConfiguration(I2SDmaTd[i], I2S_TRANSFER_SIZE, I2SDmaTd[i + 1u],
                                (TD_INC_SRC_ADR | I2S_DMA_TD_TERMOUT_EN));
    }
    /* Chain last and 1st DMA transfer descriptors to make cyclic buffer. */
    CyDmaTdSetConfiguration(VdacOutDmaTd_L[NUM_OF_BUFFERS - 1u], TRANSFER_SIZE, VdacOutDmaTd_L[0u],
                                (TD_INC_SRC_ADR | VDAC_DMA_TD_TERMOUT_EN));
    CyDmaTdSetConfiguration(VdacOutDmaTd_R[NUM_OF_BUFFERS - 1u], TRANSFER_SIZE, VdacOutDmaTd_R[0u],
                                (TD_INC_SRC_ADR));
    CyDmaTdSetConfiguration(I2SDmaTd[NUM_OF_BUFFERS - 1u], I2S_TRANSFER_SIZE, I2SDmaTd[0u],
                                (TD_INC_SRC_ADR | I2S_DMA_TD_TERMOUT_EN));


    for (i = 0u; i < NUM_OF_BUFFERS; i++)
    {
        /* Set source and destination addresses. */
        CyDmaTdSetAddress(VdacOutDmaTd_L[i], LO16((uint32) &soundBuffer_L[i * TRANSFER_SIZE]),
                                           LO16((uint32) VDAC8_L_Data_PTR));
        CyDmaTdSetAddress(VdacOutDmaTd_R[i], LO16((uint32) &soundBuffer_R[i * TRANSFER_SIZE]),
                                           LO16((uint32) VDAC8_R_Data_PTR));
        CyDmaTdSetAddress(I2SDmaTd[i], LO16((uint32) &soundBuffer_I2S[i * I2S_TRANSFER_SIZE]),
                                           LO16((uint32) I2S_TX_CH0_F0_PTR));
    }

    /* Set 1st transfer descriptor to execute. */
    CyDmaChSetInitialTd(VdacOutDmaCh_L, VdacOutDmaTd_L[0u]);
    CyDmaChSetInitialTd(VdacOutDmaCh_R, VdacOutDmaTd_R[0u]);
    CyDmaChSetInitialTd(I2SDmaCh, I2SDmaTd[0u]);

    /* Start DMA completion interrupt. */
    VdacDmaDone_StartEx(&VdacDmaDone);

    /* Stop SampleClk before start DMAs */
    SampleClk_Stop();
    IDAC_Stop();
    
    /* Start DMA operation. */
    CyDmaChEnable(VdacOutDmaCh_L, VDAC_DMA_ENABLE_PRESERVE_TD);
    CyDmaChEnable(VdacOutDmaCh_R, VDAC_DMA_ENABLE_PRESERVE_TD);            
    CyDmaChEnable(I2SDmaCh, I2S_DMA_ENABLE_PRESERVE_TD);            

    /* Enable global interrupts. */
    CyGlobalIntEnable; 
    
    /* Start LCD display for indication. */
    I2C_CharLCD_Start();
    CharLCD_Start();

    /* Start PGAs */
    PGA_L_Start();
    PGA_R_Start();
    
    /* Start PGA for 3v3 generation. */
    I2S_3v3_Start();

    /* Start I2S */
    I2S_Start();
    I2S_EnableTx();

    /* Start USBFS Operation with 5V operation. */
    USBFS_Start(USBFS_AUDIO_DEVICE, USBFS_5V_OPERATION);

    /* Wait for device enumeration. */
    while (0u == USBFS_GetConfiguration())
    {
    }

    for(;;)
    {
        /* Check if configuration or interface settings are changed. */
        if (0u != USBFS_IsConfigurationChanged()) {
            sprintf(dbuf,"[USB Configuration Changed]\n");dp(dbuf);
            
            /* Check active alternate setting. */
            if ( (0u != USBFS_GetConfiguration()) && (0u != USBFS_GetInterfaceSetting(AUDIO_INTERFACE)) ) {
                /* Alternate settings 1: Audio is streaming. */
          
                /* Reset variables. */
                syncDma  = 0u;
                skipNextOut = 0u;
                adjustIntervalCount = 0u;
                distAverage = 0u;
                lastDistAverage = distAverage;
                distAvDiff = 0;

                /* Reset VDAC output level. */
                VDAC8_L_Data=128u;
                VDAC8_R_Data=128u;

                /* Stop DMA clock */
                SampleClk_Stop();
                IDAC_Stop();

                /* Enable OUT endpoint to receive audio stream. */
                USBFS_EnableOutEP(OUT_EP_NUM);

                sprintf(dbuf,"Initial clockDivider=%d\n",initialClockDivider);dp(dbuf);
                sprintf(dbuf,"BUFFER_SIZE=%d (%d)\n",BUFFER_SIZE,sHALF_BUFFER_SIZE);dp(dbuf);
                sprintf(dbuf,"Sizeof(EZI2C_buf)=%d\n",sizeof(EZI2C_buf));dp(dbuf);
                sprintf(dbuf,"initialVctrl=%d\n",initialVctrl);dp(dbuf);
                sprintf(dbuf,"Vctrl=%d\n",vctrl);dp(dbuf);
                sprintf(dbuf,"initialIdac=%d\n",initialIdac);dp(dbuf);
                sprintf(dbuf,"Idac=%d\n",idac);dp(dbuf);

                CharLCD_Position(0u, 0u);
                CharLCD_PrintString("Audio ON ");
                //PrintEvent_StartEx(&PrintEvent);
                sprintf(dbuf,"Audio [ON].\n");dp(dbuf);
            } else {
                /* Alternate settings 0: Audio is not streaming (mute). */
            
                /* Stop DMA clock and cancel pending transfer. */
                SampleClk_Stop();
                IDAC_Stop();
                
                CyDmaClearPendingDrq(VdacOutDmaCh_L);
                CyDmaClearPendingDrq(VdacOutDmaCh_R);
                CyDmaChSetRequest(VdacOutDmaCh_L,CPU_TERM_TD);
                CyDmaChSetRequest(VdacOutDmaCh_R,CPU_TERM_TD);

                /* Find current TD. */
                CyDmaChStatus(VdacOutDmaCh_L,&td,NULL);
                for (i = 0u; i < NUM_OF_BUFFERS; ++i) {
                    if (VdacOutDmaTd_L[i] == td) break;
                }                
                outIndex = i;
                inIndex = outIndex*TRANSFER_SIZE;
                distAverage = dist = BUFFER_SIZE/2u;;
            
                /* Reset VDAC output level. */
                VDAC8_L_Data=128u;
                VDAC8_R_Data=128u;
                
                CharLCD_Position(0u, 0u);
                CharLCD_PrintString("Audio OFF");
                //PrintEvent_Disable();
                sprintf(dbuf,"Audio [OFF].\n");dp(dbuf);
            }

            /* Check if sample frequency is changed by host. */
            if ((USBFS_frequencyChanged != 0u) && (USBFS_transferState == USBFS_TRANS_STATE_IDLE)) {
                USBFS_frequencyChanged = 0u;
                
                /* Get current sampling frequency. */
                fs = USBFS_currentSampleFrequency[OUT_EP_NUM][0] +
                (USBFS_currentSampleFrequency[OUT_EP_NUM][1]<<8) +
                (USBFS_currentSampleFrequency[OUT_EP_NUM][2]<<16);

                /* Reset divider of SampleClk. */
                initialClockDivider = 64000000/(2*fs*I2S_WORD_SELECT);
                clockDivider = initialClockDivider;
                clockAdjust = 0;                    
                SampleClk_SetDividerRegister(clockDivider,0u);

                vctrlAdjMax = Vctrl_HIGH;
                vctrlAdjMin = Vctrl_LOW;
                
                sprintf(dbuf,"%4.1fkHz",fs/1000.0);
                CharLCD_Position(1u, 0u);
                CharLCD_PrintString(dbuf);
                sprintf(dbuf,"Clock changed to %4.1fkHz. new div=%d\n",fs/1000.0,initialClockDivider);dp(dbuf);
            }
        }

        /* Check if EP buffer is full. */
        if (USBFS_OUT_BUFFER_FULL == USBFS_GetEPState(OUT_EP_NUM)) {
            if (0u == skipNextOut) {
                readSize = USBFS_GetEPCount(OUT_EP_NUM);
                /* Trigger DMA to copy data from OUT endpoint buffer. */
                USBFS_ReadOutEP(OUT_EP_NUM, tmpEpBuf, readSize);

                /* Wait until DMA completes copying data from OUT endpoint buffer. */
                while (USBFS_OUT_BUFFER_FULL == USBFS_GetEPState(OUT_EP_NUM));

                /* Enable OUT endpoint to receive data from host. */
                USBFS_EnableOutEP(OUT_EP_NUM);

                /* Separate 2-channel data and append into each buffer. */
                for (j=0u;j<readSize/(AUDIO_CH*BYTES_PER_CH);j++) {
                    soundBuffer_L[inIndex] = tmpEpBuf[BYTES_PER_CH*(AUDIO_CH*j + 1) -1]+128u;
                    soundBuffer_R[inIndex] = tmpEpBuf[BYTES_PER_CH*(AUDIO_CH*j + 2) -1]+128u;
                    soundBuffer_I2S[inIndex*I2S_DATA_SIZE+0] = tmpEpBuf[BYTES_PER_CH*AUDIO_CH*j +1];
                    soundBuffer_I2S[inIndex*I2S_DATA_SIZE+1] = tmpEpBuf[BYTES_PER_CH*AUDIO_CH*j +0];
                    soundBuffer_I2S[inIndex*I2S_DATA_SIZE+2] = tmpEpBuf[BYTES_PER_CH*AUDIO_CH*j +3];
                    soundBuffer_I2S[inIndex*I2S_DATA_SIZE+3] = tmpEpBuf[BYTES_PER_CH*AUDIO_CH*j +2];
                    inIndex = (inIndex+1) % BUFFER_SIZE;
                }
                
                dist=(inIndex - outIndex*TRANSFER_SIZE+BUFFER_SIZE)%BUFFER_SIZE;

                if (syncDma==0u) {
                    distAverage = dist;
                    sprintf(dbuf,"rd=%d i=%d o=%d dist=%d\n",readSize,inIndex,outIndex*TRANSFER_SIZE,dist);dp(dbuf);
                }
                
                /* Enable DMA transfers when sound buffer is half-full. */
                if ((0u == syncDma) && (dist >= sHALF_BUFFER_SIZE)) {
                    /* Disable underflow delayed start. */
                    syncDma = 1u;

                    SampleClk_Start();
                    IDAC_Start();
                    
                    CyDmaChStatus(VdacOutDmaCh_L,&td,NULL);
                    sprintf(dbuf,"DMA Clock START div=%d td=%d dist=%d\n",clockDivider,td,
                        (inIndex-outIndex*TRANSFER_SIZE+BUFFER_SIZE)%BUFFER_SIZE);dp(dbuf);
                }
            } else {
                /* Ignore received data from host and arm OUT endpoint
                * without reading if overflow is detected.
                */
                USBFS_EnableOutEP(OUT_EP_NUM);
                skipNextOut = 0u;

                //clockDivider--; 
                //clockDivider=(clockDivider<1400u) ? 1400u : clockDivider;
                //SampleClk_SetDividerRegister(clockDivider,0u);
                //sprintf(dbuf,"DROP div=%d\n",clockDivider);dp(dbuf);
            }

            dist=(inIndex - outIndex*TRANSFER_SIZE+BUFFER_SIZE)%BUFFER_SIZE;
            //distAverage = ((long)distAverage*99ul + (long)dist*1ul)/(long)100ul;
            distAverage = distAverage*(1-MovingAverageFactor) + dist*MovingAverageFactor;

            if (syncDma==0u) {
                usb_drop_count=0;
                dma_stop_count=0;
            } else {
                #if 1
                /* Buffer over-run */
                if (usb_drop_count>0) {
                    dma_usb_count++;
                }
                usb_drop_count=0;
                
                /* Buffer under-run */
                if (dma_stop_count>0) {
                    dma_usb_count--;
                }
                dma_stop_count=0;
                #endif
            
                /* Over-run for long time */
                if (dma_usb_count>8) {
                    //idacAdj += 4;
                    //idac += (idac<(255-1)) ? 1 : 0;
                    //IDAC_SetValue(idac);
                    #if 0
                    IDACadjustIntervalCount=0u;
                    lastDistAverage=1;
                    distAverage=TRANSFER_SIZE*9;
                    adjustIntervalCount=adjustInterval;
                    #endif
                    //lastDistAverage=0;
                    #if 1
                    //if (lastSwap!=1 && vctrlAdjMax > vctrlAdjMin) {
                    if (vctrlAdjMax > vctrlAdjMin && (vctrl-128) > vctrlAdjMin ) {
                        vctrlAdjMax = vctrl;
                        //vctrlAdj = vctrlAdjMin + (vctrlAdjMax-vctrlAdjMin)*0.1 - initialVctrl;
                        vctrlAdj -= 128;
                        vctrl = initialVctrl+vctrlAdj;
                        vctrl_set(vctrl);
                        sprintf(dbuf,"U Min=%d Max=%d V=%d\n",vctrlAdjMin,vctrlAdjMax,vctrl);dp(dbuf);
                        lastSwap=1;
                    }
                    dma_usb_count = 8;
                    //dp("U");
                    //sprintf(dbuf,"U VC I=%d V=%d\r",idac,vctrl);dp(dbuf);
                    #endif
                }

                /* Under-run for long time */
                if (dma_usb_count<-8) {
                    //idacAdj -= 4;
                    //idac -= (idac>(5+1)) ? 1 : 0;
                    //IDAC_SetValue(idac);
                    #if 0
                    IDACadjustIntervalCount=0u;
                    lastDistAverage=TRANSFER_SIZE*NUM_OF_BUFFERS;
                    distAverage=TRANSFER_SIZE*1;
                    adjustIntervalCount=adjustInterval;
                    #endif
                    //lastDistAverage=0;
                    //if (lastSwap!=-1 && vctrlAdjMin < vctrlAdjMax) {
                    if (vctrlAdjMin < vctrlAdjMax && (vctrl+128) < vctrlAdjMax ) {
                        vctrlAdjMin = vctrl;
                        //vctrlAdj = vctrlAdjMin + (vctrlAdjMax-vctrlAdjMin)*0.9 - initialVctrl;
                        vctrlAdj += 128;
                        vctrl = initialVctrl+vctrlAdj;
                        vctrl_set(vctrl);
                        sprintf(dbuf,"D Min=%d Max=%d V=%d\n",vctrlAdjMin,vctrlAdjMax,vctrl);dp(dbuf);
                        lastSwap=-1;
                    }
                    dma_usb_count = -8;
                    //dp("D");
                    //sprintf(dbuf,"VC I=%d V=%d\r",idac,vctrl);dp(dbuf);
                }
            }

            /* Internal sampling clock adjustment. */
            if (++adjustIntervalCount>=adjustInterval) {
                if (lastDistAverage==0) lastDistAverage = distAverage;
                distAvDiff = distAverage - lastDistAverage;
                
                /* If buffered size is over half and still increasing, then faster the clock (decrease the divider). */
                if ( distAverage > (sHALF_BUFFER_SIZE+UpperAdjustRange) && ((distAvDiff>=0) +0) ) {
                    //idacAdj += adjustTic;
                    vctrlAdj -= adjustTic;
                    clockAdjust -= adjustTic;
                }
                /* If buffered size is under half and still decreasing, then slower the clock (increase the divider). */
                if ( distAverage < (sHALF_BUFFER_SIZE+LowerAdjustRange) && ((distAvDiff<=0) +0) ) {
                    //idacAdj -= adjustTic;
                    vctrlAdj += adjustTic;
                    clockAdjust += adjustTic;
                }

                #if 0
                uint8 tmpIdac = initialIdac+(int8)idacAdj;
                if (idac != tmpIdac) {
                    idac = tmpIdac;
                    idac = (idac<5) ? 5 : idac;
                    idac = (idac>250) ? 250 : idac;
                    IDAC_SetValue(idac);
                    sprintf(dbuf,"idac=%d vdac=%d\n",idac,vdac);dp(dbuf);
                }
                #endif
            
                uint16 tmpVctrl = initialVctrl+vctrlAdj;
                #if 0
                if (tmpVctrl<=Vctrl_LOW) {
                    vctrlAdj += 5;
                    idac += (idac<254) ? 1 : 0;
                    IDAC_SetValue(idac);
                    dp("+");
                }
                if (tmpVctrl>=Vctrl_HIGH) {
                    vctrlAdj -= 5;
                    idac -= (idac>5) ? 1 : 0;
                    IDAC_SetValue(idac);
                    dp("-");
                }
                #endif

                tmpVctrl = initialVctrl+vctrlAdj;
                if (vctrl != tmpVctrl) {
                    vctrl = tmpVctrl;

                    vctrl = (vctrl<Vctrl_LOWEST) ? Vctrl_LOWEST : vctrl;
                    vctrl = (vctrl>Vctrl_HIGHEST) ? Vctrl_HIGHEST : vctrl;
                    Vctrl_Write(vctrl);
                  
                    #if 0
                    if (++IDACadjustIntervalCount>=IDACadjustInterval) {
                        idac += (vdac<=200 && idac<254) ? 1 : 0;
                        idac -= (vdac>=250 && idac>5) ? 1 : 0;
                        IDAC_SetValue(idac);
                        IDACadjustIntervalCount=0u;
                    }
                    #endif

                    sprintf(dbuf,"I=%d V=%d \r",idac,vctrl);dp(dbuf);
                }

                                    
                /* If target divider value is differ from current value, then change the actual divider value. */
                uint16 tmpCD = initialClockDivider+(int16)clockAdjust;
                if (clockDivider != tmpCD) {
                    clockDivider = tmpCD;
                    /* Lmits divider value between 0.9 to 1.1 of initial value. */
                    clockDivider = (clockDivider<initialClockDivider*0.9) ? initialClockDivider*0.9 : clockDivider;
                    clockDivider = (clockDivider>initialClockDivider*1.1) ? initialClockDivider*1.1 : clockDivider;
                    SampleClk_SetDividerRegister(clockDivider,0u);
                }

                lastDistAverage = distAverage;
                adjustIntervalCount=0u;
            }
            
            /* If EZI2C is idle, then update monitoring values. */
            if ( (EZI2C_GetActivity() & EZI2C_STATUS_BUSY) == 0u ) {
                EZI2C_buf.clockDivider  = clockDivider;
                EZI2C_buf.dist          = dist;
                EZI2C_buf.distAvrerage  = distAverage;
                EZI2C_buf.distAvDiff    = distAvDiff;
                EZI2C_buf.clockAdjust   = clockAdjust;
                EZI2C_buf.L             = VDAC8_L_Data;
                EZI2C_buf.R             = VDAC8_R_Data;
                EZI2C_buf.inIndex       = inIndex;
                EZI2C_buf.outIndex      = outIndex*TRANSFER_SIZE;
                EZI2C_buf.flag          = flag;
                EZI2C_buf.idac          = idac;
                EZI2C_buf.vctrl          = vctrl;
            }
                
            /* When internal sampling clock is slower than PC traffic and buffer is likely over-run,
            * then skip next transfer from PC.
            */
            if ( dist > TRANSFER_SIZE*8u ) {
                skipNextOut = 1u;
                flag |= USB_DROP_FLAG;
                usb_drop_count++;
            } else {
                flag &= ~USB_DROP_FLAG;
            }
        }
    }
}



/*******************************************************************************
* Function Name: VdacDmaDone
********************************************************************************
*
* Summary:
*  The Interrupt Service Routine for a DMA transfer completion event. The DMA is
*  stopped when there is no data to send.
*
* Parameters:
*  None.
*
* Return:
*  None.
*
*******************************************************************************/
uint16 vdacDist;

CY_ISR(VdacDmaDone)
{
    /* Move to next buffer location and adjust to be within buffer size. */
    ++outIndex;
    outIndex = (outIndex >= NUM_OF_BUFFERS) ? 0u : outIndex;

    vdacDist = ((inIndex - outIndex*TRANSFER_SIZE+BUFFER_SIZE)%BUFFER_SIZE);

    /* When there is no data to transfer to VDAC stop DMA and wait until
	* buffer is half-full to continue operation.
    */
    if ( vdacDist < TRANSFER_SIZE*2u ) {
        /* Stop SampleClk */
        //SampleClk_Stop();
        
        flag |= DMA_STOP_FLAG;
        dma_stop_count++;
    } else {
        flag &= ~DMA_STOP_FLAG;
    }
    
}

void vctrl_set(uint16 v) {
    v = (v<Vctrl_LOWEST) ? Vctrl_LOWEST : v;
    v = (v>Vctrl_HIGHEST) ? Vctrl_HIGHEST : v;
    Vctrl_Write(v);
}

/* [] END OF FILE */
