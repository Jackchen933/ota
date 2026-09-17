/*******************************************************************************
** �ļ���: 		common.c
** �汾��  		1.0
** ��������: 	RealView MDK-ARM 5.20
** ����: 		liupeng
** ��������: 	2016-03-20
** ����:		��ʾ���˵������˵�����ʾһϵ�в���������ض������ļ���
                ִ��Ӧ�ó����Լ���ֹд����(�������Flash��д����)��
** ����ļ�:	common.h��ymodem.h
** �޸���־��	2016-03-20   �����ĵ�
*******************************************************************************/

/* ����ͷ�ļ� *****************************************************************/
#include "common.h"
#include "ymodem.h"
#include "stm32f4xx.h"
#include "usart.h"

/* 编译期占位: 本工程为 F4, 无 F1 的 IS_TIMEOUT_1MS/eTim2, 先置 0 以便通过编译 */
#ifndef IS_TIMEOUT_1MS
#define IS_TIMEOUT_1MS(timer, ms)   0
#endif
/* ���� ----------------------------------------------------------------------*/
pFunction Jump_To_Application;
uint32_t JumpAddress;
uint32_t BlockNbr = 0, UserMemoryMask = 0;
__IO uint32_t FlashProtection = 0;


/*******************************************************************************
  * @�������ƣ�Int2Str
  * @����˵������������ת���ַ���
  * @���������intnum:����
  * @���������str��ת��Ϊ���ַ���
  * @���ز�������
  * @��ʷ��¼:     
     <����>    <ʱ��>      <�޸ļ�¼>
*******************************************************************************/
void Int2Str(uint8_t* str, int32_t intnum)
{
    uint32_t i, Div = 1000000000, j = 0, Status = 0;

    for (i = 0; i < 10; i++)
    {
        str[j++] = (intnum / Div) + 48;

        intnum = intnum % Div;
        Div /= 10;
        if ((str[j-1] == '0') & (Status == 0))
        {
            j = 0;
        }
        else
        {
            Status++;
        }
    }
}

/*******************************************************************************
  * @�������ƣ�Int2Str
  * @����˵�����ַ���ת������
  * @���������inputstr:��ת�����ַ���
  * @���������intnum��ת�õ�����
  * @���ز�����ת�����
               1����ȷ
               0������
  * @��ʷ��¼:     
     <����>    <ʱ��>      <�޸ļ�¼>
*******************************************************************************/
uint32_t Str2Int(uint8_t *inputstr, int32_t *intnum)
{
    uint32_t i = 0, res = 0;
    uint32_t val = 0;

    if (inputstr[0] == '0' && (inputstr[1] == 'x' || inputstr[1] == 'X'))
    {
        if (inputstr[2] == '\0')
        {
            return 0;
        }
        for (i = 2; i < 11; i++)
        {
            if (inputstr[i] == '\0')
            {
                *intnum = val;
                //����1
                res = 1;
                break;
            }
            if (ISVALIDHEX(inputstr[i]))
            {
                val = (val << 4) + CONVERTHEX(inputstr[i]);
            }
            else
            {
                //��Ч���뷵��0
                res = 0;
                break;
            }
        }

        if (i >= 11)
        {
            res = 0;
        }
    }
    else//���10Ϊ2����
    {
        for (i = 0; i < 11; i++)
        {
            if (inputstr[i] == '\0')
            {
                *intnum = val;
                //����1
                res = 1;
                break;
            }
            else if ((inputstr[i] == 'k' || inputstr[i] == 'K') && (i > 0))
            {
                val = val << 10;
                *intnum = val;
                res = 1;
                break;
            }
            else if ((inputstr[i] == 'm' || inputstr[i] == 'M') && (i > 0))
            {
                val = val << 20;
                *intnum = val;
                res = 1;
                break;
            }
            else if (ISVALIDDEC(inputstr[i]))
            {
                val = val * 10 + CONVERTDEC(inputstr[i]);
            }
            else
            {
                //��Ч���뷵��0
                res = 0;
                break;
            }
        }
        //����10λ��Ч������0
        if (i >= 11)
        {
            res = 0;
        }
    }

    return res;
}

/*******************************************************************************
  * @�������ƣ�Int2Str
  * @����˵�����ַ���ת������
  * @���������inputstr:��ת�����ַ���
  * @���������intnum��ת�õ�����
  * @���ز�����ת�����
               1����ȷ
               0������
  * @��ʷ��¼:     
     <����>    <ʱ��>      <�޸ļ�¼>
*******************************************************************************/
/**
  * @brief  Get an integer from the HyperTerminal
  * @param  num: The inetger
  * @retval 1: Correct
  *         0: Error
  */
uint32_t GetIntegerInput(int32_t * num)
{
    uint8_t inputstr[16];

    while (1)
    {
        GetInputString(inputstr);
        if (inputstr[0] == '\0') continue;
        if ((inputstr[0] == 'a' || inputstr[0] == 'A') && inputstr[1] == '\0')
        {
            SerialPutString("User Cancelled \r\n");
            return 0;
        }

        if (Str2Int(inputstr, num) == 0)
        {
            SerialPutString("Error, Input again: \r\n");
        }
        else
        {
            return 1;
        }
    }
}

/*******************************************************************************
  * @�������ƣ�SerialKeyPressed
  * @����˵�������Գ����ն��Ƿ��а�������
  * @���������key:����
  * @�����������
  * @���ز�����1����ȷ
               0������
  * @��ʷ��¼:     
     <����>    <ʱ��>      <�޸ļ�¼>
*******************************************************************************/
uint32_t SerialKeyPressed(uint8_t *key)
{
    if ( USART_GetFlagStatus(USART1, USART_FLAG_RXNE) != RESET)
    {
        *key = (uint8_t)USART1->DR;
        return 1;
    }
    else
    {
        return 0;
    }
    //*key = USART_ReceiveChar(USART1);
}

/*******************************************************************************
  * @�������ƣ�GetKey
  * @����˵����ͨ�������жϻ�ȥ����
  * @�����������
  * @�����������
  * @���ز��������µļ���
  * @��ʷ��¼:     
     <����>    <ʱ��>      <�޸ļ�¼>
*******************************************************************************/
uint8_t GetKey(void)
{
    uint8_t key = 0;

    //�ȴ���������
    while (IS_TIMEOUT_1MS(eTim2, 6000))
    {
        if (SerialKeyPressed((uint8_t*)&key)) break;
    }
    return key;

}

/*******************************************************************************
  * @�������ƣ�SerialPutChar
  * @����˵�������ڷ���һ���ַ�
  * @���������C:�跢�͵��ַ�
  * @�����������
  * @���ز�������
  * @��ʷ��¼:     
     <����>    <ʱ��>      <�޸ļ�¼>
*******************************************************************************/
void SerialPutChar(uint8_t c)
{
	USART_SendChar(USART1,c);
//	  GPIO_SetBits(GPIOB,GPIO_Pin_9);//����ʹ��
//
////    USART_SendData(USART2, c);
////    while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET)
////    {
////    }
//	  USART_ClearFlag(USART1,USART_IT_TC);	
//	  USART_ClearFlag(USART1,USART_IT_TXE);
//
//	  USART_SendData(USART1, c);
//    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET)
//    {
//
//    }	
//		
//		GPIO_ResetBits(GPIOB,GPIO_Pin_9);//����ʹ��
}

/*******************************************************************************
  * @�������ƣ�SerialPutChar
  * @����˵�������ڷ���һ���ַ���
  * @���������*s:�跢�͵��ַ���
  * @�����������
  * @���ز�������
  * @��ʷ��¼:     
     <����>    <ʱ��>      <�޸ļ�¼>
*******************************************************************************/
void Serial_PutString(uint8_t *s)
{
//	  GPIO_SetBits(GPIOB,GPIO_Pin_9);//����ʹ��
//    while (*s != '\0')
//    {
//        SerialPutChar(*s);
//        s++;
//    }
//		GPIO_ResetBits(GPIOB,GPIO_Pin_9);//����ʹ��
}


/*******************************************************************************
  * @�������ƣ�GetInputString
  * @����˵�����Ӵ��ڻ�ȡһ���ַ���
  * @���������*s:���ַ����ĵ�ַ
  * @�����������
  * @���ز�������
  * @��ʷ��¼:     
     <����>    <ʱ��>      <�޸ļ�¼>
*******************************************************************************/
void GetInputString (uint8_t * buffP)
{
//    uint32_t bytes_read = 0;
//    uint8_t c = 0;
//    do
//    {
//        c = GetKey();
//        if (c == '\r')
//            break;
//        if (c == '\b')// Backspace ����  
//        {
//            if (bytes_read > 0)
//            {
//                SerialPutString("\b \b");
//                bytes_read --;
//            }
//            continue;
//        }
//        if (bytes_read >= CMD_STRING_SIZE )
//        {
//            SerialPutString("Command string size overflow\r\n");
//            bytes_read = 0;
//            continue;
//        }
//        if (c >= 0x20 && c <= 0x7E)
//        {
//            buffP[bytes_read++] = c;
//            SerialPutChar(c);
//        }
//    }
//    while (1);
//    SerialPutString(("\n\r"));
//    buffP[bytes_read] = '\0';
}

/*******************************************************************************
  * @�������ƣ�FLASH_PagesMask
  * @����˵��������Falshҳ
  * @���������Size:�ļ�����
  * @�����������
  * @���ز�����ҳ������
  * @��ʷ��¼:     
     <����>    <ʱ��>      <�޸ļ�¼>
*******************************************************************************/
uint32_t FLASH_PagesMask(__IO uint32_t Size)
{
    uint32_t pagenumber = 0x0;
    uint32_t size = Size;

    if ((size % PAGE_SIZE) != 0)
    {
        pagenumber = (size / PAGE_SIZE) + 1;
    }
    else
    {
        pagenumber = size / PAGE_SIZE;
    }
    return pagenumber;
}

/*******************************************************************************
  * @�������ƣ�FLASH_DisableWriteProtectionPages
  * @����˵�����Ӵ�Flashд����
  * @�����������
  * @�����������
  * @���ز�������
  * @��ʷ��¼:     
     <����>    <ʱ��>      <�޸ļ�¼>
*******************************************************************************/
void FLASH_DisableWriteProtectionPages(void)
{
//    uint32_t useroptionbyte = 0, WRPR = 0;
//    uint16_t var1 = OB_IWDG_SW, var2 = OB_STOP_NoRST, var3 = OB_STDBY_NoRST;
//    FLASH_Status status = FLASH_BUSY;
//
//    WRPR = FLASH_GetWriteProtectionOptionByte();
//
//    //�����Ƿ�д����
//    if ((WRPR & UserMemoryMask) != UserMemoryMask)
//    {
//        useroptionbyte = FLASH_GetUserOptionByte();
//
//        UserMemoryMask |= WRPR;
//
//        status = FLASH_EraseOptionBytes();
//
//        if (UserMemoryMask != 0xFFFFFFFF)
//        {
//            status = FLASH_EnableWriteProtection((uint32_t)~UserMemoryMask);
//        }
//        //�ô�ѡ�����Ƿ��б��
//        if ((useroptionbyte & 0x07) != 0x07)
//        {
//            //���±���ѡ����
//            if ((useroptionbyte & 0x01) == 0x0)
//            {
//                var1 = OB_IWDG_HW;
//            }
//            if ((useroptionbyte & 0x02) == 0x0)
//            {
//                var2 = OB_STOP_RST;
//            }
//            if ((useroptionbyte & 0x04) == 0x0)
//            {
//                var3 = OB_STDBY_RST;
//            }
//
//            FLASH_UserOptionByteConfig(var1, var2, var3);
//        }
//
//        if (status == FLASH_COMPLETE)
//        {
//            SerialPutString("Write Protection disabled...\r\n");
//
//            SerialPutString("...and a System Reset will be generated to re-load the new option bytes\r\n");
//            //ϵͳ��λ���¼���ѡ����
//            NVIC_SystemReset();
//        }
//        else
//        {
//            SerialPutString("Error: Flash write unprotection failed...\r\n");
//        }
//    }
//    else
//    {
//        SerialPutString("Flash memory not write protected\r\n");
//    }
}

/*******************************************************************************
  * @�������ƣ�Main_Menu
  * @����˵������ʾ�˵����ڳ����ն�
  * @�����������
  * @�����������
  * @���ز�������
  * @��ʷ��¼:     
     <����>    <ʱ��>      <�޸ļ�¼>
*******************************************************************************/
void Main_Menu(void)
{
// 	  FLASH_Status FLASHstatus;//����һ��flash����״̬����
//     uint8_t key = 0;
//     static uint8_t uStatus = 0;
//     BlockNbr = (BackAppAddress - 0x08000000) >> 12;


// #if defined (STM32F10X_MD) || defined (STM32F10X_MD_VL)
//     UserMemoryMask = ((uint32_t)~((1 << BlockNbr) - 1));
// #else /* USE_STM3210E_EVAL */
//     if (BlockNbr < 62)
//     {
//         UserMemoryMask = ((uint32_t)~((1 << BlockNbr) - 1));
//     }
//     else
//     {
//         UserMemoryMask = ((uint32_t)0x80000000);
//     }
// #endif /* (STM32F10X_MD) || (STM32F10X_MD_VL) */
//      //����flashд����ѡ���ֽڵ�ֵ
//     if ((FLASH_GetWriteProtectionOptionByte() & UserMemoryMask) != UserMemoryMask)
//     {
//         FlashProtection = 1;
//     }
//     else
//     {
//         FlashProtection = 0;
//     }



//      /* Download user application in the Flash */
// 		SerialDownload();   //ʹ��YModemЭ�����app bin�ļ�
// //		SerialPutString( "\r�������\n\r");//              				

// 		SerialPutString("Auto jump App \r\n");


//    }
}


/*******************************�ļ�����***************************************/
