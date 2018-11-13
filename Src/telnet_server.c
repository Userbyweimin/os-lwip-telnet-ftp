/**
  ******************************************************************************
  * @file           telent_server.c
  * @author         ��ô��
  * @brief          telent ������
  ******************************************************************************
  *
  * COPYRIGHT(c) 2018 GoodMorning
  *
  ******************************************************************************
  */
/* Includes ---------------------------------------------------*/
#include <string.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "cmsis_os.h"
#include "avltree.h"
#include "lwip/opt.h"
#include "lwip/arch.h"
#include "lwip/api.h"
#include "lwip/apps/fs.h" 

#include "shell.h"
#include "ustdio.h"
#include "fatfs.h"


/* Private macro ------------------------------------------------------------*/

#define TELNET_WILL  251
#define TELNET_WONT  252
#define TELNET_DO    253
#define TELNET_DONT  254
#define TELNET_IAC   255


#define TELNET_NORMAL	   0
#define TELNET_BIN_TRAN    1
#define TELNET_BIN_ERROR   2



#define TELNET_FILE_ADDR 0x8060000
/*
* secureCRT telnet �����ļ�������̣�
* CRT   : will bin tran ; do bin tran
* server: do bin tran
* CRT   : will bin tran 
* CRT   : <file data>
* CRT   : won't bin tran ; don't bin tran
* server: won't bin tran ; don't bin tran
* CRT   : won't bin tran 
* server: won't bin tran 
* CRT   : <string mode>
*/
	
/* Private types ------------------------------------------------------------*/




/* Private variables ------------------------------------------------------------*/

static struct telnetbuf
{
	struct netconn * conn;  //��Ӧ�� netconn ָ��
	volatile uint32_t tail; //telnet ���ݻ�����ĩβ
	char buf[TCP_MSS];    //telnet ���ݻ����� __align(4) 
}
current_telnet, //��ǰ���ڴ���� telnet ����
bind_telnet;    //���� printf/printk �� telnet ����


static struct telnetfile
{
	uint16_t skip0xff;
	uint16_t remain ;
	uint32_t addr ;
	char buf[TCP_MSS+4];
}
telnet_file;

//static char telnet_state = TELNET_NORMAL; //



/*---------------------------------------------------------------------------*/

static void telnet_puts(char * buf,uint16_t len)
{
	struct telnetbuf * putsbuf;

	if ( current_telnet.conn )     //��ǰ���ڴ��� telnet ����
		putsbuf = &current_telnet;
	else
	if ( bind_telnet.conn )      //�� printf/printk �� telnet ����
		putsbuf = &bind_telnet;
	else
		return ;
	
	if ( putsbuf->tail + len < TCP_MSS) 
	{
		memcpy(&putsbuf->buf[putsbuf->tail],buf,len);
		putsbuf->tail += len;
	}
}




/**
	* @brief    telnet_option 
	*           telnet ����
	* @param    arg �������
	* @return   void
*/
static void telnet_option(uint8_t option, uint8_t value) //telnet_option(TELNET_DONT,1)
{
	volatile uint32_t new_tail = 3 + current_telnet.tail;
	
	if ( new_tail < TCP_MSS ) 
	{
		char * buf = &current_telnet.buf[current_telnet.tail];
		*buf = (char)TELNET_IAC;
		*++buf = (char)option;
		*++buf = (char)value;
		current_telnet.tail = new_tail;
	}
}



/**
	* @brief    telnet_check_option 
	*           telnet ����ʱ��Ҫ���ظ��ͻ��˵�ѡ������
	* @param    arg �������
	* @return   void
*/
void telnet_check_option(char ** telnetbuf , uint16_t * buflen ,uint32_t * telnetstate)
{
	uint8_t iac = (uint8_t)((*telnetbuf)[0]);
	uint8_t opt = (uint8_t)((*telnetbuf)[1]);
	uint8_t val = (uint8_t)((*telnetbuf)[2]);

	if (TELNET_NORMAL == *telnetstate)
	{
		while(iac == TELNET_IAC && opt > 250 )
		{
			if (0 == val) //ֻ�ظ�����������
			{
				if (TELNET_WILL == opt)
				{
					*telnetstate = TELNET_BIN_TRAN;
					telnet_file.addr = TELNET_FILE_ADDR;
					telnet_file.remain = 0;
					telnet_file.skip0xff = 0;
					HAL_FLASH_Unlock();
				}
				else
					telnet_option(opt, val);
			}
			
			*telnetbuf += 3;
			*buflen -= 3;
			iac = (uint8_t)((*telnetbuf)[0]);
			opt = (uint8_t)((*telnetbuf)[1]);
			val = (uint8_t)((*telnetbuf)[2]);
		}
	}
	else
	{
		while(iac == TELNET_IAC && val == 0  && opt > 250 )
		{
			if (TELNET_WONT == opt) //ֻ�ظ�����������
			{
				iac = (uint8_t)((*telnetbuf)[3]);
				opt = (uint8_t)((*telnetbuf)[4]);
				val = (uint8_t)((*telnetbuf)[5]);

				if ( iac == TELNET_IAC  && opt == TELNET_DONT  && val == 0 )
				{
					HAL_FLASH_Lock();
					telnet_option(TELNET_WONT, 0);//�˳������ƴ���ģʽ
					telnet_option(TELNET_DONT, 0);//�˳������ƴ���ģʽ
					char * msg = & current_telnet.buf[current_telnet.tail];
					sprintf(msg,"\r\nGet file,size=%d bytes\r\n",telnet_file.addr-TELNET_FILE_ADDR);
					current_telnet.tail += strlen(msg);
					*telnetbuf += 3;
					*buflen -= 3;
					*telnetstate = TELNET_NORMAL;
				}
				else
					return ;
			}
			
			*telnetbuf += 3;
			*buflen -= 3;
			iac = (uint8_t)((*telnetbuf)[0]);
			opt = (uint8_t)((*telnetbuf)[1]);
			val = (uint8_t)((*telnetbuf)[2]);
		}
	}
}



/**
	* @brief    telnet_recv_file 
	*           telnet �����ļ������� flash ��
	* @param    arg �������
	* @return   void
*/
void telnet_recv_file(char * data , uint16_t len)
{
	uint8_t  * copyfrom = (uint8_t*)data ;//+ telnet_file.skip0xff;//0xff 0xff ���ְ��������������һ�� ff
	uint8_t	 * copyend = copyfrom + len ;
	uint8_t  * copyto = (uint8_t*)(&telnet_file.buf[telnet_file.remain]);
	uint32_t * value = (uint32_t*)(&telnet_file.buf[0]);
	uint32_t   size = 0;
	
	//telnet_file.skip0xff = ((uint8_t)data[len-1] == 0xff && (uint8_t)data[len-2] != 0xff);//0xff 0xff ���ְ������

	//����ļ��д��� 0xff ���� SecureCRT �ᷢ���� 0xff ����Ҫ�޳�һ��
	while(copyfrom < copyend)
	{
		*copyto++ = *copyfrom++ ;
		if (*copyfrom == 0xff) 
			++copyfrom;
	}

	size = copyto - (uint8_t*)(&telnet_file.buf[0]);
	telnet_file.remain = size & 0x03 ;//stm32f429 �� flash ��4��������д�룬����4�ֽ�������һ��д�� 
	size >>= 2; 	                  // ���� 4
	
	for(uint32_t i = 0;i < size ; ++i)
	{
		if (HAL_OK != HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,telnet_file.addr,*value))
		{
			Errors("write data error\r\n");
		}
		else
		{
			++value;
			telnet_file.addr += 4;
		}
	}
	
	if (telnet_file.remain) //�˴�ûд��������´�д
		memcpy(telnet_file.buf,&telnet_file.buf[size<<2],telnet_file.remain);
}



/**
	* @brief    telnet_recv_pro 
	*           telnet ������������
	* @param    arg �������
	* @return   void
*/
static void telnet_recv_pro(void const * arg)
{
	uint32_t state = TELNET_NORMAL ;
	struct netconn	* this_conn = (struct netconn *)arg; //��ǰ telnet ���Ӿ��	
	struct netbuf	* recvbuf;
	struct shell_buf  telnet_shell;//�½� shell ���� 
	
	char shell_bufmem[COMMANDLINE_MAX_LEN] = {0};

	telnet_shell.bufmem = shell_bufmem;
	telnet_shell.index = 0;
	telnet_shell.puts = telnet_puts;//���� telnet �� shell ��������
	
	telnet_option(TELNET_DO,1);   //�ͻ��˿�������
	//telnet_option(TELNET_DO,34);//�ͻ��˹ر���ģʽ
		
	while(ERR_OK == netconn_recv(this_conn, &recvbuf))	//����ֱ���յ�����
	{
		current_telnet.conn = this_conn;
		
		do
		{
			char *	  recvdata;
			uint16_t  datalen;
			
			netbuf_data(recvbuf, (void**)&recvdata, &datalen); //��ȡ����ָ��
			
			telnet_check_option(&recvdata,&datalen,&state);

			if (datalen)
			{
				if (TELNET_NORMAL == state)
					shell_input(&telnet_shell,recvdata,datalen);//�������� shell ����
				else
					telnet_recv_file(recvdata,datalen);
			}
		}
		while(netbuf_next(recvbuf) > 0);
		
		netbuf_delete(recvbuf);
		
		current_telnet.conn = NULL;
		
		if ( current_telnet.tail ) // ��� telnet �����������ݣ�����
		{	
			netconn_write(this_conn,current_telnet.buf,current_telnet.tail, NETCONN_COPY);
			current_telnet.tail = 0;
		}
		
		if ((!bind_telnet.conn) && (default_puts == telnet_puts)) //������ debug-info ��ȡ��Ϣ����
		{
			bind_telnet.conn = this_conn;//�� telnet ��������
			bind_telnet.tail = 0;
		}
	}

	if (this_conn == bind_telnet.conn)// �ر� telnet ʱ������������󶨣����
	{
		bind_telnet.conn = NULL;
		default_puts = NULL;
	}
	
	netconn_close(this_conn); //�ر�����
	netconn_delete(this_conn);//�ͷ����ӵ��ڴ�
	
	vTaskDelete(NULL);//���ӶϿ�ʱɾ���Լ�
}




/**
	* @brief    telnet_server_listen 
	*           telnet ��������������
	* @param    void
	* @return   void
*/
static void telnet_server_listen(void const * arg)
{
	struct netconn *conn, *newconn;
	err_t err;

	conn = netconn_new(NETCONN_TCP); //����һ�� TCP ����
	netconn_bind(conn,IP_ADDR_ANY,23); //�󶨶˿� 23 �Ŷ˿�
	netconn_listen(conn); //�������ģʽ

	for(;;)
	{
		err = netconn_accept(conn,&newconn); //������������
		
		if (err == ERR_OK) //�����ӳɹ�ʱ��������һ���̴߳��� telnet
		{
		  osThreadDef(telnet, telnet_recv_pro, osPriorityNormal, 0, 512);
		  osThreadCreate(osThread(telnet), newconn);
		}
	}
}



/**
	* @brief    telnet_idle_pro 
	*           telnet ���д�����
	* @param    void
	* @return   void
*/
void telnet_idle_pro(void)
{
	if (bind_telnet.conn && bind_telnet.tail)
	{
		netconn_write(bind_telnet.conn,bind_telnet.buf,bind_telnet.tail, NETCONN_COPY);
		bind_telnet.tail = 0;
	}
}



/**
	* @brief    telnet_erase_file 
	*           telnet ��ս��յ����ļ�
	* @param    arg �������
	* @return   void
*/
void telnet_erase_file(void * arg)
{
	uint32_t SectorError;
    FLASH_EraseInitTypeDef FlashEraseInit;
	
	FlashEraseInit.TypeErase    = FLASH_TYPEERASE_SECTORS; //�������ͣ��������� 
	FlashEraseInit.Sector       = 7;                       //0x8060000 �� F429 ����7������
	FlashEraseInit.NbSectors    = 1;                       //һ��ֻ����һ������
	FlashEraseInit.VoltageRange = FLASH_VOLTAGE_RANGE_3;   //��ѹ��Χ��VCC=2.7~3.6V֮��!!
	
	HAL_FLASH_Unlock();
	HAL_FLASHEx_Erase(&FlashEraseInit,&SectorError);
	HAL_FLASH_Lock();
	printk("done\r\n");
}



/**
	* @brief    telnet_server_init 
	*           telnet �������˳�ʼ��
	* @param    void
	* @return   void
*/
void telnet_server_init(void)
{
	current_telnet.tail = 0;
	bind_telnet.tail = 0;
	
	shell_register_command("telnet-erase",telnet_erase_file);	
	
	osThreadDef(TelnetServer, telnet_server_listen, osPriorityNormal, 0, 128);
	osThreadCreate(osThread(TelnetServer), NULL);//��ʼ���� lwip �󴴽�tcp��������������
}



