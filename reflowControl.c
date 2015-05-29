/**********************************************************************************************
 * reflowControl.c
 * RPI <-> 430BOOST-ADS1118 Thermocouple/LCD Board
 * 
 * Revision history:
 * rev. 1 - Initial version - Mar 2015 - junglejim63
 *
 * Acknowledgements:
 * Based on therm.c from http://www.element14.com/community/community/raspberry-pi/raspberrypi_projects/blog/2014/11/14/temperature-measurement-for-lab-and-science-projects
 * Based on spidev.c,
 * TI source code by Wayne Xu and
 * GPIO example by Gert van Loo and Dom http://elinux.org/RPi_Low-level_peripherals#C_2
 *
 * Description:
 *
 * Syntax examples:
 *
 * Connections:
 * TI board       RPI B+
 * ------------   ------------------
 * P1_1  VCC      1     3.3V
 * P1_7  CLK      23    CLK
 * P1_8  ADS_CS   26    SPI_CE1
 * P2_8  LCD_CS   24    SPI_CE0
 * P2_9  LCD_RS   11    GPIO_17_GEN0
 * P2_1  GND      9     GND
 * P2_6  SIMO     19    MOSI
 * P2_7  SOMI     21    MISO
 * P1_2  BUZZER   15    GPIO22
 ************************************************************************************************/


// include files
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <linux/spi/spidev.h>
#include <unistd.h> // sleep
#include <time.h>
#include <sys/mman.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <math.h>
#include <sys/fcntl.h>
#include <errno.h>
#include <ctype.h>

// definitions
#define DBG_PRINT 0
#define BCM2708_PERI_BASE        0x20000000
#define GPIO_BASE                (BCM2708_PERI_BASE + 0x200000) /* GPIO controller */
#define PAGE_SIZE (4*1024)
#define BLOCK_SIZE (4*1024)

#define ADS1118_TS			   (0x0010)    
#define ADS1118_PULLUP     	(0x0008)
#define ADS1118_NOP     	   (0x0002)  
#define ADS1118_CNVRDY     	(0x0001)
//Set the configuration to AIN0/AIN1, FS=+/-0.256, SS, DR=128sps, PULLUP on DOUT
#define ADSCON_CH0		      (0x8B8A)
//Set the configuration to AIN2/AIN3, FS=+/-0.256, SS, DR=128sps, PULLUP on DOUT
#define ADSCON_CH1		      (0xBB8A)
#define NSAMPLES 20
#define MS_SAMPLES 20

#define INTERNAL_SENSOR 0
#define EXTERNAL_SIGNAL 1
#define BUFSIZE 64
#define LCD_RS_GPIO  17
#define SSR1_GPIO    23
#define SSR2_GPIO    24
#define BUZZER_GPIO  22

//Defines for PID controller
#define MANUAL 0
#define AUTO 1
#define FLOAT0 0.001

//Mains PWM cycle in nanoseconds
#define MAINS_FREQ   60
#define BILLION 1000000000

// GPIO setup macros. Always use INP_GPIO(x) before using OUT_GPIO(x) or SET_GPIO_ALT(x,y)
#define INP_GPIO(g) *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3))
#define OUT_GPIO(g) *(gpio+((g)/10)) |=  (1<<(((g)%10)*3))
#define SET_GPIO_ALT(g,a) *(gpio+(((g)/10))) |= (((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))
// sets   bits which are 1 ignores bits which are 0
#define GPIO_SET *(gpio+7)
// clears bits which are 1 ignores bits which are 0
#define GPIO_CLR *(gpio+10)
// 0 if LOW, (1<<g) if HIGH
#define GET_GPIO(g) (*(gpio+13)&(1<<g))
// Pull up/pull down
#define GPIO_PULL *(gpio+37)
// Pull up/pull down clock
#define GPIO_PULLCLK0 *(gpio+38)

// typedefs
typedef struct spi_ioc_transfer spi_t;

typedef struct {
  unsigned int channel;
  int n;
  double M;
  double S;
  double value;
  double stdev;
  double max;
  double min;
} Measurement;

typedef struct {
  int beep_mode;
  int mode;
  double pv;
  double out;
  double sp;
  double Kp;
  double Ti;
  double Td;
  double Bias;
  double outHi;
  double outLo;
  double SPHi;
  double SPLo;
  struct timespec lastTime;
  double lastLastError;
  double lastError;
  double lastKp;
  double lastTi;
  double lastTd;
  double lastIntegral;
  double lastLooptime;
  int lastMode;
  Measurement tc1;
  Measurement tc2;
  int spiSemaphore; 
} Controller;

// global variables
int  mem_fd;
void *gpio_map;
volatile unsigned *gpio;
extern int errno;
static const char *device0 = "/dev/spidev0.0";
static const char *device1 = "/dev/spidev0.1";
int ads_fd;
int lcd_fd;
spi_t spi;
int dofile;
FILE* outfile;
int lcd_initialised=0;
uint8_t spi_bits = 8;
//uint32_t spi_speed = 2621440;
uint32_t spi_speed = 3932160;
unsigned char txbuf[BUFSIZE];
unsigned char rxbuf[BUFSIZE];
int local_comp;
// Controller variables
unsigned long PWMCycle_ns = BILLION/2/MAINS_FREQ; //should yield 120 cycles per second (US)


// function prototypes


// functions
int delay_ms(unsigned int msec) {
  int ret;
  struct timespec a;
  if (msec>999) {
    //fprintf(stderr, "delay_ms error: delay value needs to be less than 999\n");
    msec=999;
  }
  a.tv_nsec=((long)(msec))*1E6d;
  a.tv_sec=0;
  if ((ret = nanosleep(&a, NULL)) != 0)
  {
    //fprintf(stderr, "delay_ms error: %s\n", strerror(errno));
  }
  return(0);
}

//pauses until time is nsec after the time stored in sleepPTR
int sync_clock_delay(long nsec, struct timespec* sleepPTR) {
  struct timespec lastSleep;
  long carry;
  lastSleep = *sleepPTR; //make copy of last sleep time
  sleepPTR->tv_nsec += nsec % BILLION;
  sleepPTR->tv_sec += nsec / BILLION;
  // if result is nanoseconds greater than a billion, correct both nanosecs and secs
  if (sleepPTR->tv_nsec >= BILLION) {
     carry = sleepPTR->tv_nsec / BILLION;
     sleepPTR->tv_nsec -= BILLION;
     sleepPTR->tv_sec += carry;
  }
  clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, sleepPTR, NULL);
  return(0);
}

// Set up a memory regions to access GPIO
void setup_io() {
   /* open /dev/mem */
   if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
      printf("{\"item\": \"error\", \"value\": \"cannot open /dev/mem\"}\n");
      exit(-1);
   }

   /* mmap GPIO */
   gpio_map = mmap(
      NULL,             //Any adddress in our space will do
      BLOCK_SIZE,       //Map length
      PROT_READ|PROT_WRITE,// Enable reading & writting to mapped memory
      MAP_SHARED,       //Shared with other processes
      mem_fd,           //File to map
      GPIO_BASE         //Offset to GPIO peripheral
   );

   close(mem_fd); //No need to keep mem_fd open after mmap

   if (gpio_map == MAP_FAILED) {
      printf("{\"item\": \"error\", \"value\": \"mmap error %d\"}\n", (int)gpio_map);//errno also set!
      exit(-1);
   }

   // Always use volatile pointer!
   gpio = (volatile unsigned *)gpio_map;
}

int spi_open(int* f_desc, int sel, uint8_t config) {
	uint8_t spi_bits = 8;
	int ret;
	
	if (sel)
		*f_desc=open(device1, O_RDWR);
	else
		*f_desc=open(device0, O_RDWR);
	if (*f_desc<0)
	{
		fprintf(stderr, "Error opening device: %s\n", strerror(errno));
		return(-1);
  }
	ret=ioctl(*f_desc, SPI_IOC_WR_MODE, &config);
	if (ret<0)
  {
  	fprintf(stderr, "Error setting SPI write mode: %s\n", strerror(errno));
		return(-1);
  }
	ret=ioctl(*f_desc, SPI_IOC_RD_MODE, &config);
  if (ret<0)
  {
  	fprintf(stderr, "Error setting SPI read mode: %s\n", strerror(errno));
		return(-1);
  }
  ret=ioctl(*f_desc, SPI_IOC_WR_BITS_PER_WORD, &spi_bits);
  if (ret<0)
  {
  	fprintf(stderr, "Error setting SPI write bits: %s\n", strerror(errno));
		return(-1);
  }
  ret=ioctl(*f_desc, SPI_IOC_RD_BITS_PER_WORD, &spi_bits);
  if (ret<0)
  {
  	fprintf(stderr, "Error setting SPI read bits: %s\n", strerror(errno));
		return(-1);
  }
  ret=ioctl(*f_desc, SPI_IOC_WR_MAX_SPEED_HZ, &spi_speed);
  if (ret<0)
  {
  	fprintf(stderr, "Error setting SPI write speed: %s\n", strerror(errno));
		return(-1);
  }
  ret=ioctl(*f_desc, SPI_IOC_RD_MAX_SPEED_HZ, &spi_speed);
  if (ret<0)
  {
  	fprintf(stderr, "Error setting SPI read speed: %s\n", strerror(errno));
		return(-1);
  }	
	
	return(0);
}

// write command to LCD
void lcd_writecom(unsigned char c) {
   int ret;
   GPIO_CLR = 1<<LCD_RS_GPIO; //set RS low for transmitting command
   txbuf[0]=c;
   spi.len=1;
   spi.delay_usecs=0;
   spi.speed_hz=spi_speed;
   spi.bits_per_word=spi_bits;
   spi.cs_change=0;
   spi.tx_buf=(unsigned long)txbuf;
   spi.rx_buf=(unsigned long)rxbuf;

   ret=ioctl(lcd_fd, SPI_IOC_MESSAGE(1), &spi);
   if (ret<0) {
      fprintf(stderr, "Error performing SPI exchange: %s\n", strerror(errno));
      exit(1);
   }
}

//write data
void lcd_writedata(unsigned char c)	{
   int ret;
   GPIO_SET = 1<<LCD_RS_GPIO; //set RS high for writing data
   txbuf[0]=c;
   spi.len=1;
   spi.delay_usecs=0;
   spi.speed_hz=spi_speed;
   spi.bits_per_word=spi_bits;
   spi.cs_change=0;
   spi.tx_buf=(unsigned long)txbuf;
   spi.rx_buf=(unsigned long)rxbuf;

   ret=ioctl(lcd_fd, SPI_IOC_MESSAGE(1), &spi);
   if (ret<0) {
      fprintf(stderr, "Error performing SPI exchange: %s\n", strerror(errno));
      exit(1);
   }
}

void lcd_clear(void) {
   lcd_writecom(0x01);
   delay_ms(2);
   lcd_writecom(0x02);
   delay_ms(2);
}

/******************************************************************************
function: void lcd_display_string(unsigned char L ,unsigned char *ptr)
introduction: display a string on singal line of LCD.
parameters:L is the display line, 0 indicates the first line, 1 indicates the second line
return value:
*******************************************************************************/
void lcd_display_string(unsigned char line_num, char *ptr, int *semaphorePtr) {
   while (*semaphorePtr != 0) {delay_ms(1);}
   *semaphorePtr = 2;
	if (line_num==0) {		//first line
		lcd_writecom(0x80);
	} else if (line_num==1) {	//second line
		lcd_writecom(0xc0);
	}

	while (*ptr) {
		lcd_writedata(*ptr++);
	}
   *semaphorePtr = 0;
}

// initialize and clear the display
void lcd_init(void) {
	GPIO_SET = 1<<LCD_RS_GPIO;
	lcd_writecom(0x30);	//wake up
	lcd_writecom(0x39);	//function set
	lcd_writecom(0x14);	//internal osc frequency
	lcd_writecom(0x56);	//power control
	lcd_writecom(0x6D);	//follower control
	lcd_writecom(0x70);	//contrast
	lcd_writecom(0x0C);	//display on
	lcd_writecom(0x06);	//entry mode
	lcd_writecom(0x01);	//clear
	delay_ms(20);
}

// Send four bytes (two config bytes repeated twice, and return two bytes)
int therm_transact(void) {
	int ret;

   txbuf[0]=txbuf[0] | 0x80;

   spi.len=4;
   txbuf[2]=txbuf[0];
   txbuf[3]=txbuf[1];

   spi.delay_usecs=0;
   spi.speed_hz=spi_speed;
   spi.bits_per_word=spi_bits;
   spi.cs_change=0;
   spi.tx_buf=(unsigned long)txbuf;
   spi.rx_buf=(unsigned long)rxbuf;

   if (DBG_PRINT)  
      printf("sending [%02x %02x %02x %02x]. ", txbuf[0], txbuf[1], txbuf[2], txbuf[3]);

   ret=ioctl(ads_fd, SPI_IOC_MESSAGE(1), &spi);
   if (ret<0) {
      fprintf(stderr, "Error performing SPI exchange: %s\n", strerror(errno));
      exit(1);
   }
  
   if (DBG_PRINT)
      printf("received [%02x %02x]\n", rxbuf[0], rxbuf[1]);
   ret=rxbuf[0];
   ret=ret<<8;
   ret=ret | rxbuf[1];
   return(ret);
}

/******************************************************************************
 * function: local_compensation(int local_code)
 * introduction:
 * this function transform internal temperature sensor code to compensation code, which is added to thermocouple code.
 * local_data is at the first 14bits of the 16bits data register.
 * So we let the result data to be divided by 4 to replace right shifting 2 bits
 * for internal temperature sensor, 32 LSBs is equal to 1 Celsius Degree.
 * We use local_code/4 to transform local data to n* 1/32 degree.
 * the local temperature is transformed to compensation code for thermocouple directly.
 *                                                   (Tin -T[n-1])
 * comp codes = Code[n-1] + (Code[n] - Code[n-1])* {---------------}
 *													              (T[n] - T[n-1])
 * for example: 5-10 degree the equation is as below
 *
 * tmp = (0x001A*(local_temp - 5))/5 + 0x0019;
 *
 * 0x0019 is the 'Code[n-1]' for 5 Degrees; 	0x001A = (Code[n] - Code[n-1])
 * (local_temp - 5) is (Tin -T[n-1]);			denominator '5' is (T[n] - T[n-1])
 *
 * the compensation range of local temperature is 0-125.
 * parameters: local_code, internal sensor result
 * return value: compensation codes
 ******************************************************************************/
int local_compensation(int local_code) {
	float tmp,local_temp;
	int comp;
	local_code = local_code / 4;
	local_temp = (float)local_code / 32;	//

	if (local_temp >=0 && local_temp <=5)		//0~5
	{
		tmp = (0x0019*local_temp)/5;
		comp = tmp;
	}
	else if (local_temp> 5 && local_temp <=10)	//5~10
	{
		tmp = (0x001A*(local_temp - 5))/5 + 0x0019 ;
		comp = tmp;
	}
	else if (local_temp> 10 && local_temp <=20)	//10~20
	{
		tmp = (0x0033*(local_temp - 10))/10 + 0x0033 ;
		comp = tmp;
	}
	else if (local_temp> 20 && local_temp <=30)	//20~30
	{
		tmp = (0x0034*(local_temp - 20))/10 + 0x0066 ;
		comp = tmp;
	}
	else if (local_temp> 30 && local_temp <=40)	//30~40
	{
		tmp = (0x0034*(local_temp - 30))/10 + 0x009A ;
		comp = tmp;
	}
	else if (local_temp> 40 && local_temp <=50)	//40~50
	{
		tmp = (0x0035*(local_temp - 40))/10 + 0x00CE;
		comp = tmp;
	}

	else if (local_temp> 50 && local_temp <=60)	//50~60
	{
		tmp = (0x0035*(local_temp - 50))/10 + 0x0103;
		comp = tmp;
	}
	else if (local_temp> 60 && local_temp <=80)	//60~80
	{
		tmp = (0x006A*(local_temp - 60))/20 + 0x0138;
		comp = tmp;
	}
	else if (local_temp> 80 && local_temp <=125)//80~125
	{
		tmp = (0x00EE*(local_temp - 80))/45 + 0x01A2;
		comp = tmp;
	}
	else
	{
		comp = 0;
	}
	return comp;
}

/******************************************************************************
 * function: adc_code2temp(int code)
 * introduction:
 * this function is used to convert ADC result codes to temperature.
 * converted temperature range is 0 to 500 Celsius degree
 * Omega Engineering Inc. Type K thermocouple is used, seebeck coefficient is about 40uV/Degree from 0 to 1000 degree.
 * ADC input range is +/-256mV. 16bits. so 1 LSB = 7.8125uV. the coefficient of code to temperature is 1 degree = 40/7.8125 LSBs.
 * Because of nonlinearity of thermocouple. Different coefficients are used in different temperature ranges.
 * the voltage codes is transformed to temperature as below equation
 * 							      (Codes - Code[n-1])
 * T = T[n-1] + (T[n]-T[n-1]) * {---------------------}
 * 							     (Code[n] - Code[n-1])
 *
 * parameters: code
 * return value: far-end temperature
*******************************************************************************/
int adc_code2temp(int code) {	// transform ADC code for far-end to temperature.
	float temp;
	int t;

	temp = (float)code;

	if (code > 0xFF6C && code <=0xFFB5)			//-30~-15
	{
		temp = (float)(15 * (temp - 0xFF6C)) / 0x0049 - 30.0f;
	}
	else if (code > 0xFFB5 && code <=0xFFFF)	//-15~0
	{
		temp = (float)(15 * (temp - 0xFFB5)) / 0x004B - 15.0f;
	}
	else if (code >=0 && code <=0x0019)			//0~5
	{
		temp = (float)(5 * (temp - 0)) / 0x0019;
	}
	else if (code >0x0019 && code <=0x0033)		//5~10
	{
		temp = (float)(5 * (temp - 0x0019)) / 0x001A + 5.0f;
	}
	else if (code >0x0033 && code <=0x0066)		//10~20
	{
		temp = (float)(10 * (temp - 0x0033)) / 0x0033 + 10.0f;
	}
	else if (code > 0x0066 && code <= 0x009A)	//20~30
	{
		temp = (float)(10 * (temp - 0x0066)) / 0x0034 + 20.0f;
	}
	else if (code > 0x009A && code <= 0x00CE)	//30~40
	{
		temp = (float)(10 * (temp - 0x009A)) / 0x0034 + 30.0f;
	}
	else if ( code > 0x00CE && code <= 0x0103)	//40~50
	{
		temp = (float)(10 * (temp - 0x00CE)) / 0x0035 + 40.0f;
	}
	else if ( code > 0x0103 && code <= 0x0138)	//50~60
	{
		temp = (float)(10 * (temp - 0x0103)) / 0x0035 + 50.0f;
	}
	else if (code > 0x0138 && code <=0x01A2)	//60~80
	{
		temp = (float)(20 * (temp - 0x0138)) / 0x006A + 60.0f;
	}
	else if (code > 0x01A2 && code <= 0x020C)	//80~100
	{
		temp = (float)((temp - 0x01A2) * 20)/ 0x06A + 80.0f;
	}
	else if (code > 0x020C && code <= 0x02DE)	//100~140
	{
		temp = (float)((temp - 0x020C) * 40)/ 0x0D2 + 100.0f;
	}
	else if (code > 0x02DE && code <= 0x03AC)	//140~180
	{
		temp = (float)((temp - 0x02DE) * 40)/ 0x00CE + 140.0f;
	}
	else if (code > 0x03AC && code <= 0x0478)	//180~220
	{
		temp = (float)((temp - 0x03AB) * 40) / 0x00CD + 180.0f;
	}
	else if (code > 0x0478 && code <= 0x0548)	//220~260
	{
		temp = (float)((temp - 0x0478) * 40) / 0x00D0 + 220.0f;
	}
	else if (code > 0x0548 && code <= 0x061B)	//260~300
	{
		temp = (float)((temp - 0x0548) * 40) / 0x00D3 + 260.0f;
	}
	else if (code > 0x061B && code <= 0x06F2)	//300~340
	{
		temp = (float)((temp - 0x061B) * 40) /  0x00D7 + 300.0f;
	}
	else if (code > 0x06F2 && code <= 0x07C7)	//340~400
	{
		temp =(float) ((temp - 0x06F2) *  40)  / 0x00D5 + 340.0f;
	}
	else if (code > 0x07C7 && code <= 0x089F)	//380~420
	{
		temp =(float) ((temp - 0x07C7) * 40)  / 0x00D8 + 380.0f;
	}

	else if (code > 0x089F && code <= 0x0978)	//420~460
	{
		temp = (float)((temp - 0x089F) * 40) / 0x00D9 + 420.0f;
	}
	else if (code > 0x0978 && code <=0x0A52)	//460~500
	{
		temp =(float)((temp - 0x0978) * 40) / 0x00DA + 460.0f;
	}
	else
	{
		temp = 0xA5A5;
	}

	t = (int)(10*temp);

	return t;
}

/******************************************************************************
 * function: ads_config (unsigned int mode) (based on TI code)
 * introduction: configure and start conversion.
 * parameters:
 * mode = 0 (INTERNAL_SENSOR), ADS1118 is set to convert the voltage of integrated temperature sensor.
 * mode = 1 (EXTERNAL_SIGNAL), ADS1118 is set to convert the voltage of thermocouple.
 * chan = 0 or 1 (ADC channel)
 * return value:
*******************************************************************************/
void ads_config(unsigned int mode, unsigned int chan) {
	unsigned int tmp;
	int ret;
	
	if(chan) {
		if (mode==EXTERNAL_SIGNAL)		// Set the configuration to AIN0/AIN1, FS=+/-0.256, SS, DR=128sps, PULLUP on DOUT
			tmp = ADSCON_CH1;
		else
			tmp = ADSCON_CH1 + ADS1118_TS;// internal temperature sensor mode.DR=8sps, PULLUP on DOUT
	} else {
		if (mode==EXTERNAL_SIGNAL)		// Set the configuration to AIN0/AIN1, FS=+/-0.256, SS, DR=128sps, PULLUP on DOUT
			tmp = ADSCON_CH0;
		else
			tmp = ADSCON_CH0 + ADS1118_TS;// internal temperature sensor mode.DR=8sps, PULLUP on DOUT
	}

	txbuf[0]=(unsigned char)((tmp>>8) & 0xff);
	txbuf[1]=(unsigned char)(tmp & 0xff);
	ret=therm_transact();
}

/******************************************************************************
 * function: ads_read(unsigned int mode)
 * introduction: read the ADC result and start a new conversion.
 * parameters:
 * mode = 0 (INTERNAL_SENSOR), ADS1118 is set to convert the voltage of integrated temperature sensor.
 * mode = 1 (EXTERNAL_SIGNAL), ADS1118 is set to convert the voltage of thermocouple.
 * chan = 0 or 1 (ADC channel)
 * return value:result of last conversion
 */
int ads_read(unsigned int mode, unsigned int chan) {
	unsigned int tmp;
	int result;

	if(chan) {
		if (mode==EXTERNAL_SIGNAL)		// Set the configuration to AIN0/AIN1, FS=+/-0.256, SS, DR=128sps, PULLUP on DOUT
			tmp = ADSCON_CH1;
		else
			tmp = ADSCON_CH1 + ADS1118_TS;// internal temperature sensor mode.DR=8sps, PULLUP on DOUT
	} else {
		if (mode==EXTERNAL_SIGNAL)		// Set the configuration to AIN0/AIN1, FS=+/-0.256, SS, DR=128sps, PULLUP on DOUT
			tmp = ADSCON_CH0;
		else
			tmp = ADSCON_CH0 + ADS1118_TS;// internal temperature sensor mode.DR=8sps, PULLUP on DOUT
	}

	txbuf[0]=(unsigned char)((tmp>>8) & 0xff);
	txbuf[1]=(unsigned char)(tmp & 0xff);
	result=therm_transact();

	return(result);
}

// returns the measured temperature
double get_measurement(unsigned int channel,  int* semaphorePtr) {
	int result;
	int local_data;
	double result_d;
	
	ads_config(INTERNAL_SENSOR,channel);  // start internal sensor measurement
	delay_ms(10);
   while (*semaphorePtr != 0) {delay_ms(1);}
   *semaphorePtr = 1;
	local_data=ads_read(EXTERNAL_SIGNAL,channel); // read internal sensor measurement and start external sensor measurement
   *semaphorePtr = 0;
	delay_ms(10);
   while (*semaphorePtr != 0) {delay_ms(1);}
   *semaphorePtr = 1;
	result=ads_read(EXTERNAL_SIGNAL,channel); // read external sensor measurement and restart external sensor measurement
   *semaphorePtr = 0;
	
	local_comp = local_compensation(local_data);
	result = result + local_comp;
	result=result & 0xffff;
	result = adc_code2temp(result);
	
	//printf("10x temp is %d\n", result);
	result_d=((double)result)/10;
	
	return(result_d);
}

double get_measurement_fast(unsigned int channel,  int* semaphorePtr) {
	int result;
	double result_d;
	
   while (*semaphorePtr != 0) {delay_ms(1);}
   *semaphorePtr = 1;
	result=ads_read(EXTERNAL_SIGNAL,channel); // read external sensor measurement and restart external sensor measurement
   *semaphorePtr = 0;
	result = result + local_comp;
	result=result & 0xffff;
	result = adc_code2temp(result);
	
	result_d=((double)result)/10;
	
	return(result_d);
}

// Convert the integer portion of unix timestamp into H:M:S
void unixtime2string(char* int_part, char* out_time) {
	unsigned int nutime;
	struct tm *nts;
	char buf1[100];
	char buf2[50];
	
	sscanf(int_part, "%u", &nutime);
	nts=localtime((time_t*)&nutime);
	strftime(buf1, 100, "%H:%M:%S", nts);
	
	strcpy(out_time, buf1);	
}
void upcase(char *s) {
    while (*s) {
        *s = toupper(*s);
        s++;        
    }
}
/***************************************************************************
Limit function - limits value to within min and max
***************************************************************************/
double limit(double minimum, double value, double maximum) {
   return fmax(minimum, fmin(value, maximum));
}
/***************************************************************************
Non-Blocking Getline
   This function is not reliable, but I left it in in case I figure somethign out in the future.
   Instead of this, I forked a child process that uses blocking getline.
***************************************************************************/
ssize_t nb_getline(char **lineptr, size_t *n, FILE *stream) {
   ssize_t getLineResult = -1;
   fd_set rfds;
   struct timeval tv;
   int retval;

   /* Watch stdin (fd 0) to see when it has input. */
   FD_ZERO(&rfds);
   FD_SET(0, &rfds);

   /* Poll STDIN */
   tv.tv_sec = 0;
   tv.tv_usec = 0;
   retval = select(1, &rfds, NULL, NULL, &tv); //if select returns a value,
   if (retval) {
      getLineResult = getline(lineptr, n, stream); // data should be in buffer for getline
   }
   return getLineResult;
}

/***************************************************************************
BEEP - turn on or off beeper based on beepmode
***************************************************************************/
int BEEP(int beepmode) {
   static unsigned long beepcount = 0;
   beepcount++;
   switch (beepmode) {
      case 1: //short beep - 0.1 sec
         if (beepcount < MAINS_FREQ/5) {
            GPIO_CLR = 1<<BUZZER_GPIO; //turn on buzzer
         } else {
            beepmode = 0;
         }
         break;
      case 2: //long beep - 1 sec
         if (beepcount < MAINS_FREQ*2) {
            GPIO_CLR = 1<<BUZZER_GPIO; //turn on buzzer
         } else {
            beepmode = 0;
         }
         break;
      case 3: //3 medium beeps - .5 sec beep and .25 sec pause
         if ((beepcount < MAINS_FREQ*2/2) ||
             ((beepcount >= MAINS_FREQ*3/2) && (beepcount < MAINS_FREQ*5/2)) ||
             ((beepcount >= MAINS_FREQ*6/2) && (beepcount < MAINS_FREQ*8/2)) ) {
            GPIO_CLR = 1<<BUZZER_GPIO; //turn on buzzer
         } else if (((beepcount >= MAINS_FREQ*2/2) && (beepcount < MAINS_FREQ*3/2)) ||
             ((beepcount >= MAINS_FREQ*5/2) && (beepcount < MAINS_FREQ*6/2)) ) {
            GPIO_SET = 1<<BUZZER_GPIO;  //turn off buzzer
         } else {
            beepmode = 0;
         }
         break;
      default: //
         beepcount = 0;
         GPIO_SET = 1<<BUZZER_GPIO;
         break;
   }
   return beepmode;
}
/***************************************************************************
Elapsed Time
***************************************************************************/
double elapsed(struct timespec *start, struct timespec *end) {
   return ((double) (end->tv_sec - start->tv_sec)) + 
               ((double)((long)end->tv_nsec - (long)start->tv_nsec)/(double)BILLION);
}

/***************************************************************************
Print measurement as JSON
***************************************************************************/
void printMeasurementJSON(Measurement* m) {
  printf("{\"item\": \"TC\"");
  printf(", \"channel\": %d", m->channel);
  printf(", \"n\": %d", m->n);
  printf(", \"M\": %f", m->M);
  printf(", \"S\": %f", m->S);
  printf(", \"value\": %f", m->value);
  printf(", \"stdev\": %f", m->stdev);
  printf(", \"max\": %f", m->max);
  printf(", \"min\": %f", m->min);
  printf("}\n");
}

/***************************************************************************
Print structure as JSON
***************************************************************************/
void printJSON(Controller* c) {
  printf("{\"item\": \"Controller\"");
  printf(", \"beep_mode\": %d", c->beep_mode);
  printf(", \"mode\": %d", c->mode);
  printf(", \"pv\": %f", c->pv);
  printf(", \"out\": %f", c->out);
  printf(", \"sp\": %f", c->sp);
  printf(", \"Kp\": %f", c->Kp);
  printf(", \"Ti\": %f", c->Ti);
  printf(", \"Td\": %f", c->Td);
  printf(", \"Bias\": %f", c->Bias);
  printf(", \"outHi\": %f", c->outHi);
  printf(", \"outLo\": %f", c->outLo);
  printf(", \"SPHi\": %f", c->SPHi);
  printf(", \"SPLo\": %f", c->SPLo);
  printf(", \"lastLastError\": %f", c->lastLastError);
  printf(", \"lastError\": %f", c->lastError);
  printf(", \"lastKp\": %f", c->lastKp);
  printf(", \"lastTi\": %f", c->lastTi);
  printf(", \"lastTd\": %f", c->lastTd);
  printf(", \"lastintegral\": %f", c->lastIntegral);
  printf(", \"lastLooptime\": %f", c->lastLooptime);
  printf(", \"lastMode\": %d", c->lastMode);
  printf("}\n");
}

/***************************************************************************
PID Controller Initialization
***************************************************************************/
void initializePID(Controller* c) {
  c->beep_mode = 0;
  c->mode = MANUAL;
  c->pv = 20.0;
  c->out = 0.0;
  c->sp = 20.0;
  c->Kp = 1.0;
  c->Ti = 60.0;
  c->Td = 0.0;
  c->Bias = 0.0;
  c->outHi = 100.0;
  c->outLo = 0.0;
  c->SPHi = 300.0;
  c->SPLo = 0.0;
  clock_gettime(CLOCK_MONOTONIC, &c->lastTime);
  c->lastLastError = 0.0;
  c->lastError = 0.0;
  c->lastKp = 1.0;
  c->lastTi = 60.0;
  c->lastTd = 0.0;
  c->lastIntegral = 0.0;
  c->lastLooptime = 0.0;
  c->lastMode = MANUAL;
     //initialize thermocouple measurement structures
  c->tc1.channel = 0;
  c->tc1.n = 0;
  c->tc1.M = 0.0;
  c->tc1.S = 0.0;
  c->tc1.value = 0.0;
  c->tc1.stdev = 0.0;
  c->tc1.max = 0.0;
  c->tc1.min = 0.0;
  c->tc2.channel = 1;
  c->tc2.n = 0;
  c->tc2.M = 0.0;
  c->tc2.S = 0.0;
  c->tc2.value = 0.0;
  c->tc2.stdev = 0.0;
  c->tc2.max = 0.0;
  c->tc2.min = 0.0;
  //initialize spi semaphore - keeps LCD and ADC from being written to at same time.
  c->spiSemaphore = 0;
}

/***************************************************************************
PID Controller
***************************************************************************/
void pidControl(Controller* c) {
   struct timespec timenow;
   double looptime, error, dError, iError, integral, out, Kp;
   
   //get time since last run
   clock_gettime(CLOCK_MONOTONIC, &timenow);
   looptime = elapsed(&c->lastTime, &timenow);
   looptime = (looptime < FLOAT0) ? FLOAT0 : looptime; //make sure looptime is not 0
   
   //calculate errors
   error = c->sp - c->pv;
   dError = (error - c->lastError)*c->Td/looptime;
   integral = 0.0;
   Kp = limit(FLOAT0, c->Kp, c->Kp); //make sure working gain is not zero
   c->lastKp = limit(FLOAT0, c->lastKp, c->lastKp); //make sure gain history is not zero
   
   if (c->mode == MANUAL) {
      out = c->out;
      c->sp = c->pv; //SP track in MAN
   } else if (c->mode == AUTO && c->Ti < FLOAT0) { //PD controller
      out = Kp*(error + dError) + c->Bias;
   } else { //PID controller
      iError = error*looptime/c->Ti;
      if (c->lastMode == MANUAL || c->Ti < FLOAT0) { // bumpless reset integral, mode just switched from Manual or PD only
         c->lastIntegral = c->out/Kp - error - iError - dError;
      } else if (Kp != c->lastKp || c->Ti != c->lastTi || c->Td != c->lastTd) { // adjust previous integral so tuning changes are bumpless 
         c->lastIntegral = c->out/c->Kp - c->lastError - c->lastError*looptime/c->Ti - (c->lastError - c->lastLastError)*c->Td/looptime;
      }
      integral = c->lastIntegral + iError;
      out = Kp*(error + integral + iError + dError);
      if (out > c->outHi) {
         integral = c->outHi/Kp - error - iError - dError;
      } else if (out < c->outLo) {
         integral = c->outLo/Kp - error - iError - dError;
      }      
   }
   //set output
   c->out = limit(c->outLo, out, c->outHi);
   
   //Save values for next iteration
   c->lastTime = timenow;
   c->lastLastError = c->lastError;
   c->lastError = error;
   c->lastKp = c->Kp;
   c->lastTi = c->Ti;
   c->lastTd = c->Td;
   c->lastIntegral = integral;
   c->lastLooptime = looptime;
   c->lastMode = c->mode;
}

/***************************************************************************
Grab sample temperature and calculate running mean and variance
   see http://www.johndcook.com/blog/standard_deviation/ 
***************************************************************************/
void sampleTemp(Measurement* m, int* semaphorePtr) {
   double T, mLast;
   m->n++;
   if (m->n == 1) { //first measurement
      m->M = get_measurement(m->channel, semaphorePtr); //get_measurement first gets internal junction temp then gets TC (20ms delays)
      m->S = 0.0;
      m->max = m->M;
      m->min = m->M;
   } else {
      T = get_measurement_fast(m->channel, semaphorePtr); //get_measurement_fast just grabs TC value and starts next TC sampling
      mLast = m->M;
      m->M = mLast + (T - mLast)/m->n; //running mean
      m->S = m->S + (T - mLast) * (T - m->M);
      m->max = fmax(T, m->max);
      m->min = fmin(T, m->min);
   }
}

/***************************************************************************
Average 
***************************************************************************/
double sampleMean(Measurement* m) {
   return (m->n > 0) ? m->M : 0.0;
}

/***************************************************************************
Variance 
***************************************************************************/
double sampleVariance(Measurement* m) {
   return (m->n > 1) ? m->S/(m->n - 1) : 0.0;
}

/***************************************************************************
Standard Deviation 
***************************************************************************/
double sampleStddev(Measurement* m) {
   return sqrt(sampleVariance(m));
}

/***************************************************************************
shutdown logic - turn off all pins, stop SPI devices, 
***************************************************************************/
void shutdown() {
   //close ads
   int localSemaphore = 0;
   close(ads_fd);
   //turn off LCD
   lcd_clear();
	lcd_display_string(0,"   Shut down", &localSemaphore);
	lcd_display_string(1,"*** Goodbye ***", &localSemaphore);
   close(lcd_fd);
   //turn off GPIO pins by setting them back to inputs
   INP_GPIO(SSR1_GPIO); //reset output pin to High-Z input
   INP_GPIO(BUZZER_GPIO); //reset output pin to High-Z input

   exit(0);
}

/***************************************************************************
Signal handler for Main
***************************************************************************/
void sig_handler_main(int signo) {
   int status;
   char *signame = strdup(strsignal(signo));
   upcase(signame);
   printf("{\"item\": \"processMessage\", \"msgsource\": \"main\", \"signal\": {\"name\":\"%s\", \"number\":%d}}\n", signame, signo);
   free(signame);
   //wait for child process to end
   wait(&status);
   
   shutdown();
}

/***************************************************************************
Signal handler for Child process
***************************************************************************/
void sig_handler_child(int signo) {
   char *signame = strdup(strsignal(signo));
   upcase(signame);
   printf("{\"item\": \"processMessage\", \"msgsource\": \"child\", \"signal\": {\"name\":\"%s\", \"number\":%d}}\n", signame, signo);
   free(signame);
   exit(1);
}

/***************************************************************************
Process Commands received by Main
***************************************************************************/
void command(char* input, Controller* ctlPtrCmd) {
   // char cmd[100];
   char *cmd;
   cmd = malloc(100 * sizeof(char));
   int int1;
   float float1;
   
   input[strlen(input)-1] = 0;  //strip off \n
   sscanf(input, "%s", cmd);
   //printf("Command: %s\n", cmd);
   if (strcmp(cmd, "beep_mode") == 0) {
      sscanf(input, "%s %d", cmd, &int1);
      ctlPtrCmd->beep_mode = int1;
      printf("{\"item\": \"Command\", \"command\": \"beep_mode\", \"value\": %d, \"string\": \"%s\"}\n", int1, input);
   } else if (strcmp(cmd, "lcd0") == 0) {
      lcd_display_string(0, input + 5, &ctlPtrCmd->spiSemaphore);
      printf("{\"item\": \"Command\", \"command\": \"lcd0\", \"value\": \"%s\", \"string\": \"%s\"}\n", input + 5, input);
   } else if (strcmp(cmd, "lcd1") == 0) {
      lcd_display_string(1, input + 5, &ctlPtrCmd->spiSemaphore);
      printf("{\"item\": \"Command\", \"command\": \"lcd1\", \"value\": \"%s\", \"string\": \"%s\"}\n", input + 5, input);
   } else if (strcmp(cmd, "mode") == 0) {
      sscanf(input, "%s %d", cmd, &int1);
      ctlPtrCmd->mode = int1;
      printf("{\"item\": \"Command\", \"command\": \"mode\", \"value\": %d, \"string\": \"%s\"}\n", int1, input);
   } else if (strcmp(cmd, "out") == 0) {
      sscanf(input, "%s %f", cmd, &float1);
      ctlPtrCmd->out = limit(ctlPtrCmd->outLo, float1, ctlPtrCmd->outHi);
      printf("{\"item\": \"Command\", \"command\": \"out\", \"value\": %f, \"string\": \"%s\"}\n", ctlPtrCmd->out, input);
   } else if (strcmp(cmd, "sp") == 0) {
      sscanf(input, "%s %f", cmd, &float1);
      ctlPtrCmd->sp = limit(ctlPtrCmd->SPLo, float1, ctlPtrCmd->SPHi);
      printf("{\"item\": \"Command\", \"command\": \"sp\", \"value\": %f, \"string\": \"%s\"}\n", ctlPtrCmd->sp, input);
   } else if (strcmp(cmd, "Kp") == 0) {
      sscanf(input, "%s %f", cmd, &float1);
      ctlPtrCmd->Kp = float1;
      printf("{\"item\": \"Command\", \"command\": \"Kp\", \"value\": %f, \"string\": \"%s\"}\n", ctlPtrCmd->Kp, input);
   } else if (strcmp(cmd, "Ti") == 0) {
      sscanf(input, "%s %f", cmd, &float1);
      ctlPtrCmd->Ti = limit(0.0, float1, 10000.0);
      printf("{\"item\": \"Command\", \"command\": \"Ti\", \"value\": %f, \"string\": \"%s\"}\n", ctlPtrCmd->Ti, input);
   } else if (strcmp(cmd, "Td") == 0) {
      sscanf(input, "%s %f", cmd, &float1);
      ctlPtrCmd->Td = limit(0.0, float1, 300.0);
      printf("{\"item\": \"Command\", \"command\": \"Td\", \"value\": %f, \"string\": \"%s\"}\n", ctlPtrCmd->Td, input);
   } else if (strcmp(cmd, "Bias") == 0) {
      sscanf(input, "%s %f", cmd, &float1);
      ctlPtrCmd->Bias = limit(ctlPtrCmd->outLo, float1, ctlPtrCmd->outHi);
      printf("{\"item\": \"Command\", \"command\": \"Bias\", \"value\": %f, \"string\": \"%s\"}\n", ctlPtrCmd->Bias, input);
   } else if (strcmp(cmd, "outHi") == 0) {
      sscanf(input, "%s %f", cmd, &float1);
      ctlPtrCmd->outHi = limit(ctlPtrCmd->outLo + FLOAT0, float1, 10000.0);
      printf("{\"item\": \"Command\", \"command\": \"outHi\", \"value\": %f, \"string\": \"%s\"}\n", ctlPtrCmd->outHi, input);
   } else if (strcmp(cmd, "outLo") == 0) {
      sscanf(input, "%s %f", cmd, &float1);
      ctlPtrCmd->outLo = limit(0.0, float1, ctlPtrCmd->outHi - FLOAT0);
      printf("{\"item\": \"Command\", \"command\": \"outLo\", \"value\": %f, \"string\": \"%s\"}\n", ctlPtrCmd->outLo, input);
   } else if (strcmp(cmd, "SPHi") == 0) {
      sscanf(input, "%s %f", cmd, &float1);
      ctlPtrCmd->SPHi = limit(ctlPtrCmd->SPLo + FLOAT0, float1, 1000000.0);
      printf("{\"item\": \"Command\", \"command\": \"SPHi\", \"value\": %f, \"string\": \"%s\"}\n", ctlPtrCmd->SPHi, input);
   } else if (strcmp(cmd, "SPLo") == 0) {
      sscanf(input, "%s %f", cmd, &float1);
      ctlPtrCmd->SPLo = limit(-1000000.0, float1, ctlPtrCmd->SPHi - FLOAT0);
      printf("{\"item\": \"Command\", \"command\": \"SPLo\", \"value\": %f, \"string\": \"%s\"}\n", ctlPtrCmd->SPLo, input);
   } else {
      printf("{\"item\": \"Command\", \"command\": \"INVALID\", \"value\": null, \"string\": \"%s\"}\n", input);
   }
   return;
}

/***************************************************************************
Child main program
***************************************************************************/
int child_main(Controller* ctlPtrChild) {
   struct timespec lastSleep;
   long PWMcounter;
   long beepCounter;
   int PWM;
      
   //make sure output is not buffered (this program generally a child to a nodejs program) and set signal handler
   setbuf(stdout, NULL);
   setbuf(stderr, NULL);
   signal(SIGINT, sig_handler_child);
   signal(SIGTERM, sig_handler_child);
   
   PWMcounter = 0;
   beepCounter = 0;
   clock_gettime(CLOCK_MONOTONIC, &lastSleep);  //capture current time as time of 'last sleep'
   while (1) {
      sync_clock_delay(PWMCycle_ns, &lastSleep);
      PWM = (int)(ctlPtrChild->out + 0.5);
      PWMcounter++;
      // PWMcounter %= 100;
      if (PWMcounter == 100) {
         // printf("PWM: %d, Out: %f\n",PWM, ctlPtrChild->out);
         PWMcounter = 0;
         pidControl(ctlPtrChild); //run PID algorithm
         printJSON(ctlPtrChild); //send data out formatted as JSON
      }
      if (PWMcounter < PWM) {
         GPIO_SET = 1<<SSR1_GPIO; //turn on SSR pin
      } else {
         GPIO_CLR = 1<<SSR1_GPIO; //turn off SSR pin
         // printf("SSR Off. PWM: %d, PWMcounter: %d, Out: %f\n", PWM, PWMcounter, ctlPtrChild->out);
      }
      //Manage beeper
      ctlPtrChild->beep_mode = BEEP(ctlPtrChild->beep_mode);
   }
}

/***************************************************************************
STDIN Management Child main program
   this process waits for stdin input via Blocking Getline, then processes the input 
   and waits for the next input command.  Could not fins a way around this that was reliable.
   Tried to watch the file via 'select' to see when input was available, this was not reliable
   when many commands were sent quickly.  Tried changing stdin to non blocking via O_NONBLOCK:
     fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK)
   but this missed input lines.  No idea why I couldn't get this way to work. 
   Finally settled on a separate process.  I hate Blocking stdin reads... 2 days of hell...
***************************************************************************/
int stdin_main(Controller* ctlPtrChild) {
   //variables for input string
   char *inputLine = NULL;
   size_t inputLineLength = 0;
   ssize_t inputCharCount;
   // char temp[200];
   
   for(;;) {
      inputCharCount = getline(&inputLine, &inputLineLength, stdin);
      // strcpy(temp, inputLine);
      // temp[strlen(temp)-1] = 0;  //strip off \n
      // printf("InputLine: %s, InputCharCount: %d, InpuLineLength: %d, Error: %d\n",temp, inputCharCount, inputLineLength, errno);
      if (inputCharCount == -1) {break;}
      // printf("INPUT RECEIVED: %s, count: %d\n", temp, inputCharCount);
      command(inputLine, ctlPtrChild);
   }
}

/***************************************************************************
main program
***************************************************************************/
int main(int argc, char* argv[]) {
   /***************************************************************************
   Initialize variables
   ***************************************************************************/
   int ret;
   unsigned int n = 0;
   //----- SET SPI MODE -----
   //SPI_MODE_0 (0,0) 	CPOL = 0, CPHA = 0, Clock idle low, data is clocked in on rising edge, output data (change) on falling edge
   //SPI_MODE_1 (0,1) 	CPOL = 0, CPHA = 1, Clock idle low, data is clocked in on falling edge, output data (change) on rising edge
   //SPI_MODE_2 (1,0) 	CPOL = 1, CPHA = 0, Clock idle high, data is clocked in on falling edge, output data (change) on rising edge
   //SPI_MODE_3 (1,1) 	CPOL = 1, CPHA = 1, Clock idle high, data is clocked in on rising, edge output data (change) on falling edge
   uint8_t lcd_spi_config = SPI_MODE_0;
 	uint8_t ads_spi_config = SPI_MODE_1;
   int ShmID;
   pid_t pid_stdin;
   pid_t pid_pwm;
   
   Controller *ctlPTR;  //this is the key structure that holds all the shared data.
   
   /***************************************************************************
   setup shared memory
   ***************************************************************************/
   ShmID = shmget(IPC_PRIVATE, sizeof(Controller), IPC_CREAT | 0666);
   if (ShmID < 0) {
		fprintf(stderr,"reflowControl Exiting - could not initialize shared memory area (shmget)\n");
      exit(1);
   }
   ctlPTR = (Controller *) shmat(ShmID, NULL, 0); //assign controller 
   if ((void *) ctlPTR == (void *) -1) {
		fprintf(stderr,"reflowControl Exiting - could not attach to shared memory area (shmat)\n");
      exit(1);
   }
   initializePID(ctlPTR);
   
   /***************************************************************************
   setup ADS, LCD, other GPIO
   ***************************************************************************/
   sleep(2); //sleep for 2 seconds at start to make sure Node program has started listening for IO streams
   //make sure output is not buffered (this program generally a child to a nodejs program) and set signal handler
   setbuf(stdout, NULL); //set stdout to not buffer output
   setbuf(stderr, NULL); //set stderr to not buffer output
   signal(SIGINT, sig_handler_main);  //capture signal interrupt ^C to cleanly shutdown
   signal(SIGTERM, sig_handler_main); //capture process termination signal to cleanly shutdown
   //Initialize GPIO - see http://elinux.org/RPi_Low-level_peripherals#C_2
   // Set up gpi pointer for direct register access
   setup_io();
   // Set GPIO pin 23 SSR1_GPIO to output
   INP_GPIO(SSR1_GPIO); // must use INP_GPIO before we can use OUT_GPIO
   OUT_GPIO(SSR1_GPIO);
   GPIO_CLR = 1<<SSR1_GPIO; //make sure output is turned off
   // Set GPIO pin 22 BUZZER_GPIO to output
   INP_GPIO(BUZZER_GPIO); // must use INP_GPIO before we can use OUT_GPIO
   OUT_GPIO(BUZZER_GPIO);
   GPIO_SET = 1<<BUZZER_GPIO; //make sure output is turned off - Buzzer is PNP, so pin high is buzzer off.
   // Set GPIO pin 17 LCD_RS_GPIO to output
   INP_GPIO(LCD_RS_GPIO); // must use INP_GPIO before we can use OUT_GPIO
   OUT_GPIO(LCD_RS_GPIO);
   // Initialize LCD
	ret = spi_open(&lcd_fd, 0, lcd_spi_config);  //initialize lcd_fd, 0 means device 0 (LCD), SPI_MODE_0
	if (ret != 0) {
		fprintf(stderr,"reflowControl Exiting - Could not initialize LCD\n");
		exit(1);
	}
	lcd_init();
	lcd_clear();					            // LCD clear
   
	lcd_display_string(0,"Reflow Start", &ctlPTR->spiSemaphore);	// display startup message
   // Initialize ADS Thermocouple chip
	ret=spi_open(&ads_fd, 1, ads_spi_config);
	if (ret != 0) {
		fprintf(stderr,"reflowControl Exiting - could not initialize ADS thermocouple chip\n");
		exit(1);
	}

   /***************************************************************************
   fork child process to manage SSR
   ***************************************************************************/
   pid_pwm = fork();
   switch(pid_pwm) {
   case -1:
		fprintf(stderr,"reflowControl Exiting - could not fork a child process for PWM (fork)\n"); exit(1);
      break;
   case 0: //child process
      child_main(ctlPTR);
      break;
   default: //main process
      printf("{\"item\": \"childProcess\", \"value\": \"PID_PWM\", \"pid\": %d}\n", pid_pwm);
      /***************************************************************************
      fork child process to manage stdin
      ***************************************************************************/
      pid_stdin = fork();
      switch(pid_stdin) {
      case -1:
         fprintf(stderr,"reflowControl Exiting - could not fork a child process for stdin (fork)\n"); exit(1);
         break;
      case 0: //child process
         stdin_main(ctlPTR);
         break;
      default: //main process
         printf("{\"item\": \"childProcess\", \"value\": \"PID_STDIN\", \"pid\": %d}\n", pid_stdin);
         while (1) {
            n++;
            delay_ms(MS_SAMPLES);
            sampleTemp(&ctlPTR->tc1, &ctlPTR->spiSemaphore);
            if (n >= NSAMPLES) {
               ctlPTR->tc1.value = sampleMean(&ctlPTR->tc1);
               ctlPTR->tc1.stdev = sampleStddev(&ctlPTR->tc1);
               printMeasurementJSON(&ctlPTR->tc1);
               ctlPTR->tc1.n = 0;
               ctlPTR->pv = ctlPTR->tc1.value;
               n = 0;
            }
         }
         break;
      }
   }
   //exit
   kill(pid_pwm, SIGKILL);
   shutdown();
   return(0);
}

