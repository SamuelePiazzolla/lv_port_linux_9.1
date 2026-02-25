#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <tml.h>
#include <linux/spi/spidev.h>

#define SPI_BUS         "/dev/spidev0.0"
#define SPI_MODE	SPI_MODE_0
#define SPI_BITS	8
#define SPI_SPEED       1000000

#define PIN_INT         10
#define PIN_ENABLE      9

#define EDGE_NONE    0
#define EDGE_RISING  1
#define EDGE_FALLING 2
#define EDGE_BOTH    3

static int iEnableFd    = 0;
static int iInterruptFd = 0;

static int SpiRead(int pDevHandle, char* pBuffer, int nBytesToRead) 
{
    int numRead = 0;
    struct spi_ioc_transfer spi[2];
    char buf = 0xFF;
    memset(spi, 0x0, sizeof(spi));
    spi[0].tx_buf = (unsigned long)&buf;
    spi[0].rx_buf = (unsigned long)NULL;
    spi[0].len = 1;
    spi[0].delay_usecs = 0;
    spi[0].speed_hz = SPI_SPEED;
    spi[0].bits_per_word = SPI_BITS;
    spi[0].cs_change = 0;
    spi[0].tx_nbits = 0;
    spi[0].rx_nbits = 0;
    spi[1].tx_buf = (unsigned long)NULL;
    spi[1].rx_buf = (unsigned long)pBuffer;
    spi[1].len = nBytesToRead;
    spi[1].delay_usecs = 0;
    spi[1].speed_hz = SPI_SPEED;
    spi[1].bits_per_word = SPI_BITS;
    spi[1].cs_change = 0;
    spi[1].tx_nbits = 0;
    spi[1].rx_nbits = 0;
    numRead = ioctl(pDevHandle, SPI_IOC_MESSAGE(2), &spi);
    if (numRead > 0) numRead -= 1;
    return numRead;
}

int tml_open(int * handle)
{
    unsigned char spi_mode = SPI_MODE;
    unsigned char spi_bitsPerWord = SPI_BITS;
    static unsigned int speed = SPI_SPEED;
    *handle = open(SPI_BUS, O_RDWR | O_NOCTTY);
    if((*handle <= 0)) goto error;
    if(ioctl(*handle, SPI_IOC_WR_MODE, &spi_mode) < 0) goto error;
    if(ioctl(*handle, SPI_IOC_RD_MODE, &spi_mode) < 0) goto error;
    if(ioctl(*handle, SPI_IOC_WR_BITS_PER_WORD, &spi_bitsPerWord) < 0) goto error;
    if(ioctl(*handle, SPI_IOC_RD_BITS_PER_WORD, &spi_bitsPerWord) < 0) goto error;
    if(ioctl(*handle, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) goto error;

    return 0;

error:
    if (*handle) close(*handle);
    return -1;
}

void tml_close(int handle)
{
    if(handle) close(handle);
}

void tml_reset(int handle)
{
    system("echo 1 > /sys/class/leds/nfc_enable/brightness");
    usleep(10 * 1000);
    system("echo 0 > /sys/class/leds/nfc_enable/brightness");
    usleep(10 * 1000);
    system("echo 1 > /sys/class/leds/nfc_enable/brightness");
}

int tml_send(int handle, char *pBuff, int buffLen)
{
    struct spi_ioc_transfer spi;
    char tx_buf[257];
    char rx_buf[257] = {0};
    int ret;
    memset(&spi, 0x0, sizeof(spi));
    tx_buf[0] = 0x7F;
    memcpy(&tx_buf[1], pBuff, buffLen);
    spi.tx_buf = (unsigned long)tx_buf;
    spi.rx_buf = (unsigned long)rx_buf;
    spi.len = buffLen+1;
    spi.delay_usecs = 0;
    spi.speed_hz = SPI_SPEED;
    spi.bits_per_word = SPI_BITS;
    spi.tx_nbits = 0;
    spi.rx_nbits = 0;
    spi.cs_change = 0;
    ret = ioctl(handle, SPI_IOC_MESSAGE(1), &spi);
    if (rx_buf[0] != 0xFF) ret =0;
    usleep(10 * 1000);
    return ret;
}

int tml_receive_polling(int handle, char *pBuff, int buffLen)
{
    int numRead = 0;
    int ret;

    // Polling loop: tenta per un certo numero di volte
    for (int i = 0; i < 200; i++) { // 200 tentativi (~200 ms)
        // Leggi header (3 byte)
        ret = SpiRead(handle, pBuff, 3);
        if (ret == 3) {
            numRead = 3;

            // Controlla lunghezza pacchetto
            if (pBuff[2] + 3 > buffLen) return 0;

            // Leggi il resto dei dati
            ret = SpiRead(handle, &pBuff[3], pBuff[2]);
            if (ret <= 0) return 0;
            numRead += ret;

            return numRead; // Ricezione completata
        }

        usleep(1000); // attesa tra i tentativi (1 ms)
    }

    return 0; // Timeout
}

int tml_receive(int handle, char *pBuff, int buffLen)
{
    return tml_receive_polling(handle, pBuff, buffLen);
}

int tml_transceive_polling(int handle, char *pTx, int TxLen, char *pRx, int RxLen)
{
    int NbBytes = 0;

    // Invia il pacchetto
    if (tml_send(handle, pTx, TxLen) == 0) {
        if (tml_send(handle, pTx, TxLen) == 0)
            return 0;
    }

    // Polling per ricezione
    while (NbBytes == 0) {
        NbBytes = tml_receive_polling(handle, pRx, RxLen);
    }

    return NbBytes;
}

int tml_transceive(int handle, char *pTx, int TxLen, char *pRx, int RxLen)
{
    return tml_transceive_polling(handle, pTx, TxLen, pRx, RxLen);

}
