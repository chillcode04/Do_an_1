#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"
#include "driver/i2s.h"
#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "esp_http_client.h"


#include "mbedtls/platform.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/esp_debug.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include <mbedtls/base64.h>
#include <sys/param.h>
#include <cJSON.h>
static const char *TAG = "example";
const char mount_point[] = "/sdcard";
esp_err_t ret;
sdmmc_card_t *card;
typedef uint8_t byte;

/*----------------------------------------WIFI--------------------------------------------------------*/
// parameters for WIFI
#define WIFI_SSID "NQH"
#define WIFI_PASS "12345679"
#define MAX_RETRY 5
static int retry_count = 0;


FILE *file;
const char file_txt[] = "/hoplab.txt";
const char file_wav[] = "/hoplab.wav";
char pathFile[64];

/*---------------------------------------I2S-------------------------------------------------------------*/
// I2S peripheral
#define I2S_WS 25
#define I2S_SD 33
#define I2S_SCK 32
#define I2S_PORT I2S_NUM_0
#define I2S_SAMPLE_RATE (16000)
#define I2S_SAMPLE_BITS (16)
#define I2S_READ_LEN (16 * 1024)
#define RECORD_TIME (2) // Seconds
#define I2S_CHANNEL_NUM (1)
#define RECORD_SIZE (I2S_CHANNEL_NUM * I2S_SAMPLE_RATE * I2S_SAMPLE_BITS / 8 * RECORD_TIME)

/*---------------------------------------SPI--------------------------------------------------------------*/
// SPI peripheral
#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK 18
#define PIN_NUM_CS 5
#define SPI_DMA_CHAN 1

//----------------------------------------GMAIL--------------------------------------------------/
//* Constants that are configurable in menuconfig */
#define MAIL_SERVER "smtp.googlemail.com"
#define MAIL_PORT "587"
#define SENDER_MAIL "ahquyendz2018@gmail.com"
#define SENDER_PASSWORD "xldc otjo pbkh pdnt"
#define RECIPIENT_MAIL "ahhuy2021p@gmail.com"

static const char *TAG_GMAIL = "smtp_example";

#define TASK_STACK_SIZE (16 * 1024)
#define BUF_SIZE 512

#define VALIDATE_MBEDTLS_RETURN(ret, min_valid_ret, max_valid_ret, goto_label) \
    do                                                                         \
    {                                                                          \
        if (ret < min_valid_ret || ret > max_valid_ret)                        \
        {                                                                      \
            goto goto_label;                                                   \
        }                                                                      \
    } while (0)
/*----------------------------------------------------------------------------------------------*/

// Semaphore để đồng bộ hóa (khai báo toàn cục)
static SemaphoreHandle_t recording_done_semaphore = NULL;
static SemaphoreHandle_t upload_semaphore = NULL;
extern const uint8_t server_root_cert_pem_start[] asm("_binary_server_root_cert_pem_start");
extern const uint8_t server_root_cert_pem_end[] asm("_binary_server_root_cert_pem_end");

static int write_and_get_response(mbedtls_net_context *sock_fd, unsigned char *buf, size_t len)
{
    int ret;
    const size_t DATA_SIZE = 128;
    unsigned char data[DATA_SIZE];
    char code[4];
    size_t i, idx = 0;

    if (len)
    {
        ESP_LOGD(TAG_GMAIL, "%s", buf);
    }

    if (len && (ret = mbedtls_net_send(sock_fd, buf, len)) <= 0)
    {
        ESP_LOGE(TAG_GMAIL, "mbedtls_net_send failed with error -0x%x", -ret);
        return ret;
    }

    do
    {
        len = DATA_SIZE - 1;
        memset(data, 0, DATA_SIZE);
        ret = mbedtls_net_recv(sock_fd, data, len);

        if (ret <= 0)
        {
            ESP_LOGE(TAG_GMAIL, "mbedtls_net_recv failed with error -0x%x", -ret);
            goto exit;
        }

        data[len] = '\0';
        printf("\n%s", data);
        len = ret;
        for (i = 0; i < len; i++)
        {
            if (data[i] != '\n')
            {
                if (idx < 4)
                {
                    code[idx++] = data[i];
                }
                continue;
            }

            if (idx == 4 && code[0] >= '0' && code[0] <= '9' && code[3] == ' ')
            {
                code[3] = '\0';
                ret = atoi(code);
                goto exit;
            }

            idx = 0;
        }
    } while (1);

exit:
    return ret;
}

static int write_ssl_and_get_response(mbedtls_ssl_context *ssl, unsigned char *buf, size_t len)
{
    int ret;
    const size_t DATA_SIZE = 128;
    unsigned char data[DATA_SIZE];
    char code[4];
    size_t i, idx = 0;

    if (len)
    {
        ESP_LOGD(TAG_GMAIL, "%s", buf);
    }

    while (len && (ret = mbedtls_ssl_write(ssl, buf, len)) <= 0)
    {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            ESP_LOGE(TAG_GMAIL, "mbedtls_ssl_write failed with error -0x%x", -ret);
            goto exit;
        }
    }

    do
    {
        len = DATA_SIZE - 1;
        memset(data, 0, DATA_SIZE);
        ret = mbedtls_ssl_read(ssl, data, len);

        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            continue;
        }

        if (ret <= 0)
        {
            ESP_LOGE(TAG_GMAIL, "mbedtls_ssl_read failed with error -0x%x", -ret);
            goto exit;
        }

        ESP_LOGD(TAG_GMAIL, "%s", data);

        len = ret;
        for (i = 0; i < len; i++)
        {
            if (data[i] != '\n')
            {
                if (idx < 4)
                {
                    code[idx++] = data[i];
                }
                continue;
            }

            if (idx == 4 && code[0] >= '0' && code[0] <= '9' && code[3] == ' ')
            {
                code[3] = '\0';
                ret = atoi(code);
                goto exit;
            }

            idx = 0;
        }
    } while (1);

exit:
    return ret;
}

static int write_ssl_data(mbedtls_ssl_context *ssl, unsigned char *buf, size_t len)
{
    int ret;

    if (len)
    {
        ESP_LOGD(TAG_GMAIL, "%s", buf);
    }

    while (len && (ret = mbedtls_ssl_write(ssl, buf, len)) <= 0)
    {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            ESP_LOGE(TAG_GMAIL, "mbedtls_ssl_write failed with error -0x%x", -ret);
            return ret;
        }
    }

    return 0;
}

static int perform_tls_handshake(mbedtls_ssl_context *ssl)
{
    int ret = -1;
    uint32_t flags;
    char *buf = NULL;
    buf = (char *)calloc(1, BUF_SIZE);
    if (buf == NULL)
    {
        ESP_LOGE(TAG_GMAIL, "calloc failed for size %d", BUF_SIZE);
        goto exit;
    }

    ESP_LOGI(TAG_GMAIL, "Performing the SSL/TLS handshake...");

    fflush(stdout);
    while ((ret = mbedtls_ssl_handshake(ssl)) != 0)
    {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            ESP_LOGE(TAG_GMAIL, "mbedtls_ssl_handshake returned -0x%x", -ret);
            goto exit;
        }
    }

    ESP_LOGI(TAG_GMAIL, "Verifying peer X.509 certificate...");

    if ((flags = mbedtls_ssl_get_verify_result(ssl)) != 0)
    {
        /* In real life, we probably want to close connection if ret != 0 */
        ESP_LOGW(TAG_GMAIL, "Failed to verify peer certificate!");
        mbedtls_x509_crt_verify_info(buf, BUF_SIZE, "  ! ", flags);
        ESP_LOGW(TAG_GMAIL, "verification info: %s", buf);
    }
    else
    {
        ESP_LOGI(TAG_GMAIL, "Certificate verified.");
    }

    ESP_LOGI(TAG_GMAIL, "Cipher suite is %s", mbedtls_ssl_get_ciphersuite(ssl));
    ret = 0; /* No error */

exit:
    if (buf)
    {
        free(buf);
    }
    return ret;
}

static void smtp_client_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Stack Email: %d", uxTaskGetStackHighWaterMark(NULL));
    char *buf = NULL;
    unsigned char base64_buffer[1024];
    int ret, len;
    size_t base64_len;

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_x509_crt cacert;
    mbedtls_ssl_config conf;
    mbedtls_net_context server_fd;

    mbedtls_ssl_init(&ssl);
    mbedtls_x509_crt_init(&cacert);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    ESP_LOGI(TAG_GMAIL, "Seeding the random number generator");

    mbedtls_ssl_config_init(&conf);

    mbedtls_entropy_init(&entropy);
    if ((ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                     NULL, 0)) != 0)
    {
        ESP_LOGE(TAG_GMAIL, "mbedtls_ctr_drbg_seed returned -0x%x", -ret);
        goto exit;
    }

    ESP_LOGI(TAG_GMAIL, "Loading the CA root certificate...");

    ret = mbedtls_x509_crt_parse(&cacert, server_root_cert_pem_start,
                                 server_root_cert_pem_end - server_root_cert_pem_start);

    if (ret < 0)
    {
        ESP_LOGE(TAG_GMAIL, "mbedtls_x509_crt_parse returned -0x%x", -ret);
        goto exit;
    }

    ESP_LOGI(TAG_GMAIL, "Setting hostname for TLS session...");

    /* Hostname set here should match CN in server certificate */
    if ((ret = mbedtls_ssl_set_hostname(&ssl, MAIL_SERVER)) != 0)
    {
        ESP_LOGE(TAG_GMAIL, "mbedtls_ssl_set_hostname returned -0x%x", -ret);
        goto exit;
    }

    ESP_LOGI(TAG_GMAIL, "Setting up the SSL/TLS structure...");

    if ((ret = mbedtls_ssl_config_defaults(&conf,
                                           MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
    {
        ESP_LOGE(TAG_GMAIL, "mbedtls_ssl_config_defaults returned -0x%x", -ret);
        goto exit;
    }

    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

    if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0)
    {
        ESP_LOGE(TAG_GMAIL, "mbedtls_ssl_setup returned -0x%x", -ret);
        goto exit;
    }

    mbedtls_net_init(&server_fd);

    ESP_LOGI(TAG_GMAIL, "Connecting to %s:%s...", MAIL_SERVER, MAIL_PORT);

    if ((ret = mbedtls_net_connect(&server_fd, MAIL_SERVER,
                                   MAIL_PORT, MBEDTLS_NET_PROTO_TCP)) != 0)
    {
        ESP_LOGE(TAG_GMAIL, "mbedtls_net_connect returned -0x%x", -ret);
        goto exit;
    }

    ESP_LOGI(TAG_GMAIL, "Connected.");

    mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

    buf = (char *)calloc(1, BUF_SIZE);
    if (buf == NULL)
    {
        ESP_LOGE(TAG_GMAIL, "calloc failed for size %d", BUF_SIZE);
        goto exit;
    }

    /* Get response */
    ret = write_and_get_response(&server_fd, (unsigned char *)buf, 0);
    VALIDATE_MBEDTLS_RETURN(ret, 200, 299, exit);

    ESP_LOGI(TAG_GMAIL, "Writing EHLO to server...");
    len = snprintf((char *)buf, BUF_SIZE, "EHLO %s\r\n", "ESP32");
    ret = write_and_get_response(&server_fd, (unsigned char *)buf, len);
    VALIDATE_MBEDTLS_RETURN(ret, 200, 299, exit);

    ESP_LOGI(TAG_GMAIL, "Writing STARTTLS to server...");
    len = snprintf((char *)buf, BUF_SIZE, "STARTTLS\r\n");
    ret = write_and_get_response(&server_fd, (unsigned char *)buf, len);
    VALIDATE_MBEDTLS_RETURN(ret, 200, 299, exit);

    ret = perform_tls_handshake(&ssl);
    if (ret != 0)
    {
        goto exit;
    }

    // Lấy độ lớn của File
    sprintf(pathFile, "%s%s", mount_point, file_wav);
    FILE *f_wav = fopen(pathFile, "r");
    if (f_wav == NULL)
    {
        ESP_LOGE(TAG_GMAIL, "Failed to open file %s", pathFile);
        goto exit;
    }
    fseek(f_wav, 0, SEEK_END);
    long file_wav_size = ftell(f_wav);
    fseek(f_wav, 0, SEEK_SET);
    printf("File %s found, size: %ld\n", pathFile, file_wav_size);

    sprintf(pathFile, "%s%s", mount_point, file_txt);
    FILE *f_txt = fopen(pathFile, "r");
    if (f_txt == NULL)
    {
        ESP_LOGE(TAG_GMAIL, "Failed to open file %s", pathFile);
        goto exit;
    }
    fseek(f_txt, 0, SEEK_END);
    long file_txt_size = ftell(f_txt);
    fseek(f_txt, 0, SEEK_SET);
    printf("File %s found, size: %ld\n", pathFile, file_txt_size);

    /* Authentication */
    ESP_LOGI(TAG_GMAIL, "Authentication...");

    ESP_LOGI(TAG_GMAIL, "Write AUTH LOGIN");
    len = snprintf((char *)buf, BUF_SIZE, "AUTH LOGIN\r\n");
    ret = write_ssl_and_get_response(&ssl, (unsigned char *)buf, len);
    VALIDATE_MBEDTLS_RETURN(ret, 200, 399, exit);

    ESP_LOGI(TAG_GMAIL, "Write USER NAME");
    ret = mbedtls_base64_encode((unsigned char *)base64_buffer, sizeof(base64_buffer),
                                &base64_len, (unsigned char *)SENDER_MAIL, strlen(SENDER_MAIL));
    if (ret != 0)
    {
        ESP_LOGE(TAG_GMAIL, "Error in mbedtls encode! ret = -0x%x", -ret);
        goto exit;
    }
    len = snprintf((char *)buf, BUF_SIZE, "%s\r\n", base64_buffer);
    ret = write_ssl_and_get_response(&ssl, (unsigned char *)buf, len);
    VALIDATE_MBEDTLS_RETURN(ret, 300, 399, exit);

    ESP_LOGI(TAG_GMAIL, "Write PASSWORD");
    ret = mbedtls_base64_encode((unsigned char *)base64_buffer, sizeof(base64_buffer),
                                &base64_len, (unsigned char *)SENDER_PASSWORD, strlen(SENDER_PASSWORD));
    if (ret != 0)
    {
        ESP_LOGE(TAG_GMAIL, "Error in mbedtls encode! ret = -0x%x", -ret);
        goto exit;
    }
    len = snprintf((char *)buf, BUF_SIZE, "%s\r\n", base64_buffer);
    ret = write_ssl_and_get_response(&ssl, (unsigned char *)buf, len);
    VALIDATE_MBEDTLS_RETURN(ret, 200, 399, exit);

    /* Compose email */
    ESP_LOGI(TAG_GMAIL, "Write MAIL FROM");
    len = snprintf((char *)buf, BUF_SIZE, "MAIL FROM:<%s>\r\n", SENDER_MAIL);
    ret = write_ssl_and_get_response(&ssl, (unsigned char *)buf, len);
    VALIDATE_MBEDTLS_RETURN(ret, 200, 299, exit);

    ESP_LOGI(TAG_GMAIL, "Write RCPT");
    len = snprintf((char *)buf, BUF_SIZE, "RCPT TO:<%s>\r\n", RECIPIENT_MAIL);
    ret = write_ssl_and_get_response(&ssl, (unsigned char *)buf, len);
    VALIDATE_MBEDTLS_RETURN(ret, 200, 299, exit);

    ESP_LOGI(TAG_GMAIL, "Write DATA");
    len = snprintf((char *)buf, BUF_SIZE, "DATA\r\n");
    ret = write_ssl_and_get_response(&ssl, (unsigned char *)buf, len);
    VALIDATE_MBEDTLS_RETURN(ret, 300, 399, exit);

    ESP_LOGI(TAG_GMAIL, "Write Content");
    /* We do not take action if message sending is partly failed. */
    len = snprintf((char *)buf, BUF_SIZE,
                   "From: %s\r\nSubject: mbed TLS Test mail\r\n"
                   "To: %s\r\n"
                   "MIME-Version: 1.0 (mime-construct 1.9)\n",
                   "ESP32 SMTP Client", RECIPIENT_MAIL);

    /**
     * Note: We are not validating return for some ssl_writes.
     * If by chance, it's failed; at worst email will be incomplete!
     */
    ret = write_ssl_data(&ssl, (unsigned char *)buf, len);

    /* Multipart boundary */
    len = snprintf((char *)buf, BUF_SIZE,
                   "Content-Type: multipart/mixed;boundary=XYZabcd1234\n"
                   "--XYZabcd1234\n");
    ret = write_ssl_data(&ssl, (unsigned char *)buf, len);

    /* Text */
    len = snprintf((char *)buf, BUF_SIZE,
                   "Content-Type: text/plain\n"
                   "This is a file sent from SD card 28/03/2025.\r\n"
                   "\r\n"
                   "Enjoy!\n\n--XYZabcd1234\n");
    ret = write_ssl_data(&ssl, (unsigned char *)buf, len);

    /* Attachment */
    // Attachment 1: File .txt từ SD card
    unsigned char file_buffer1[96 * 3];
    size_t bytes_read1;
    size_t total_read = 0;

    len = snprintf((char *)buf, BUF_SIZE,
                   "Content-Type: application/octet-stream;name=\"%s\"\n"
                   "Content-Transfer-Encoding: base64\n"
                   "Content-Disposition:attachment;filename=\"%s\"\r\n\n",
                   file_txt, file_txt);
    ret = write_ssl_data(&ssl, (unsigned char *)buf, len);

    /* Đọc và gửi file theo từng khối */

    while (total_read < file_txt_size)
    {
        bytes_read1 = fread(file_buffer1, 1, sizeof(file_buffer1), f_txt);
        if (bytes_read1 <= 0)
        {
            break;
        }
        total_read += bytes_read1;

        ret = mbedtls_base64_encode((unsigned char *)base64_buffer, sizeof(base64_buffer),
                                    &base64_len, (unsigned char *)file_buffer1, bytes_read1);
        if (ret != 0)
        {
            ESP_LOGE(TAG_GMAIL, "Error in mbedtls encode! ret = -0x%x", -ret);
            goto exit;
        }
        len = snprintf((char *)buf, BUF_SIZE, "%s", base64_buffer);
        ret = write_ssl_data(&ssl, (unsigned char *)buf, len);
    }
    printf("Total bytes read for text file: %zu\n", total_read);
    fclose(f_txt);

    // Thêm \r\n ở cuối toàn bộ dữ liệu Base64 của file .txt
    len = snprintf((char *)buf, BUF_SIZE, "\r\n");
    ret = write_ssl_data(&ssl, (unsigned char *)buf, len);

    // Thêm boundary để chuyển sang file tiếp theo
    // len = snprintf((char *)buf, BUF_SIZE, "\n--XYZabcd1234\n");
    // ret = write_ssl_data(&ssl, (unsigned char *)buf, len);

    
    // Attachment 2: File .wav từ SD card

    // total_read = 0;

    // len = snprintf((char *)buf, BUF_SIZE,
    //                "Content-Type: application/octet-stream;name=\"%s\"\n"
    //                "Content-Transfer-Encoding: base64\n"
    //                "Content-Disposition: attachment;filename=\"%s\"\r\n\n",
    //                file_wav, file_wav);
    // ret = write_ssl_data(&ssl, (unsigned char *)buf, len);

    // while (total_read < file_wav_size)
    // {
    //     bytes_read1 = fread(file_buffer1, 1, sizeof(file_buffer1), f_wav);
    //     if (bytes_read1 <= 0)
    //     {
    //         break;
    //     }
    //     total_read += bytes_read1;

    //     ret = mbedtls_base64_encode((unsigned char *)base64_buffer, sizeof(base64_buffer),
    //                                 &base64_len, (unsigned char *)file_buffer1, bytes_read1);
    //     if (ret != 0)
    //     {
    //         ESP_LOGE(TAG_GMAIL, "Error in mbedtls encode! ret = -0x%x", -ret);
    //         goto exit;
    //     }
    //     len = snprintf((char *)buf, BUF_SIZE, "%s", base64_buffer);
    //     ret = write_ssl_data(&ssl, (unsigned char *)buf, len);
    // }
    // printf("Total bytes read for audio file: %zu\n", total_read);
    // fclose(f_wav);

    // len = snprintf((char *)buf, BUF_SIZE, "\r\n");
    // ret = write_ssl_data(&ssl, (unsigned char *)buf, len);

    len = snprintf((char *)buf, BUF_SIZE, "\n--XYZabcd1234--\r\n.\r\n");
    ret = write_ssl_and_get_response(&ssl, (unsigned char *)buf, len);
    VALIDATE_MBEDTLS_RETURN(ret, 200, 299, exit);
    ESP_LOGI(TAG_GMAIL, "Email sent!");

    /* Close connection */
    mbedtls_ssl_close_notify(&ssl);
    ret = 0; /* No errors */

exit:
    mbedtls_net_free(&server_fd);
    mbedtls_x509_crt_free(&cacert);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    if (ret != 0)
    {
        mbedtls_strerror(ret, buf, 100);
        ESP_LOGE(TAG_GMAIL, "Last error was: -0x%x - %s", -ret, buf);
    }

    putchar('\n'); /* Just a new line */
    if (buf)
    {
        free(buf);
    }
    vTaskDelete(NULL);
}

const int headerSize = 44;
long file_size;
void spiInit()
{
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED
      .format_if_mount_failed = true,
#else
      .format_if_mount_failed = false,
#endif // EXAMPLE_FORMAT_IF_MOUNT_FAILED
      .max_files = 5,
      .allocation_unit_size = 16 * 1024};
  ESP_LOGI(TAG, "Initializing SD card");
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  spi_bus_config_t bus_cfg = {
      .mosi_io_num = PIN_NUM_MOSI,
      .miso_io_num = PIN_NUM_MISO,
      .sclk_io_num = PIN_NUM_CLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 4000,
  };
  ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CHAN);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to initialize bus.");
  }
  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = PIN_NUM_CS;
  slot_config.host_id = host.slot;

  ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

  if (ret != ESP_OK)
  {
    if (ret == ESP_FAIL)
    {
      ESP_LOGE(TAG, "Failed to mount filesystem. "
                    "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
    }
    else
    {
      ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                    "Make sure SD card lines have pull-up resistors in place.",
               esp_err_to_name(ret));
    }
    return;
  }
  ESP_LOGI(TAG, "Filesystem mounted");
  sdmmc_card_print_info(stdout, card);
}

void i2sInit()
{
  printf("I2S is initing\n");
  i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = I2S_SAMPLE_RATE,
      .bits_per_sample = I2S_SAMPLE_BITS,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = 0,
      .dma_buf_count = 64,
      .dma_buf_len = 1024,
      .use_apll = 1};

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);

  const i2s_pin_config_t pin_config = {
      .bck_io_num = I2S_SCK,
      .ws_io_num = I2S_WS,
      .data_out_num = -1,
      .data_in_num = I2S_SD};

  i2s_set_pin(I2S_PORT, &pin_config);
}

void i2s_adc_data_scale(uint8_t *d_buff, uint8_t *s_buff, uint32_t len)
{
  uint32_t j = 0;
  uint16_t adc_value = 0;
  for (int i = 0; i < len; i += 2)
  {
    adc_value = ((((uint16_t)(s_buff[i + 1] & 0xf) << 8) | (s_buff[i + 0])));
    uint16_t scaled_value = (uint16_t)((uint32_t)adc_value * 65536 / 4096);
    d_buff[j++] = scaled_value & 0xFF;
    d_buff[j++] = (scaled_value >> 8) & 0xFF;
  }
}

void i2s_adc(void *arg)
{
  int i2s_read_len = I2S_READ_LEN;
  int flash_wr_size = 0;
  size_t bytes_read;

  char *i2s_read_buff = (char *)calloc(i2s_read_len, sizeof(char));
  uint8_t *flash_write_buff = (uint8_t *)calloc(i2s_read_len, sizeof(char));

  i2s_read(I2S_PORT, (void *)i2s_read_buff, i2s_read_len, &bytes_read, portMAX_DELAY);
  i2s_read(I2S_PORT, (void *)i2s_read_buff, i2s_read_len, &bytes_read, portMAX_DELAY);

  ESP_LOGI(TAG, " *** Recording Start *** ");
  while (flash_wr_size < RECORD_SIZE)
  {
    // read data from I2S bus, in this case, from ADC.
    i2s_read(I2S_PORT, (void *)i2s_read_buff, i2s_read_len, &bytes_read, portMAX_DELAY);
    // save original dat a from I2S(ADC) into flash.
    i2s_adc_data_scale(flash_write_buff, (uint8_t *)i2s_read_buff, i2s_read_len);
    fwrite((const byte *)flash_write_buff, 1, i2s_read_len, file);
    flash_wr_size += i2s_read_len;
    ESP_LOGI(TAG, "Sound recording (%u%%)", flash_wr_size * 100 / RECORD_SIZE);
  }
  fclose(file);

  free(i2s_read_buff);
  i2s_read_buff = NULL;
  free(flash_write_buff);
  flash_write_buff = NULL;
  ESP_LOGI(TAG, " *** Recording Done *** ");
  ESP_LOGI(TAG, "Saved file %s%s",mount_point, file_wav);

  xSemaphoreGive(recording_done_semaphore);
  vTaskDelete(NULL);
}

void wavHeader(byte *header, int wavSize)
{
  header[0] = 'R';
  header[1] = 'I';
  header[2] = 'F';
  header[3] = 'F';
  unsigned int fileSize = wavSize + headerSize - 8;
  header[4] = (byte)(fileSize & 0xFF);
  header[5] = (byte)((fileSize >> 8) & 0xFF);
  header[6] = (byte)((fileSize >> 16) & 0xFF);
  header[7] = (byte)((fileSize >> 24) & 0xFF);
  header[8] = 'W';
  header[9] = 'A';
  header[10] = 'V';
  header[11] = 'E';
  header[12] = 'f';
  header[13] = 'm';
  header[14] = 't';
  header[15] = ' ';
  header[16] = 0x10;
  header[17] = 0x00;
  header[18] = 0x00;
  header[19] = 0x00;
  header[20] = 0x01;
  header[21] = 0x00;
  header[22] = 0x01;
  header[23] = 0x00;
  header[24] = 0x80;
  header[25] = 0x3E;
  header[26] = 0x00;
  header[27] = 0x00;
  header[28] = 0x00;
  header[29] = 0x7D;
  header[30] = 0x00;
  header[31] = 0x00;
  header[32] = 0x02;
  header[33] = 0x00;
  header[34] = 0x10;
  header[35] = 0x00;
  header[36] = 'd';
  header[37] = 'a';
  header[38] = 't';
  header[39] = 'a';
  header[40] = (byte)(wavSize & 0xFF);
  header[41] = (byte)((wavSize >> 8) & 0xFF);
  header[42] = (byte)((wavSize >> 16) & 0xFF);
  header[43] = (byte)((wavSize >> 24) & 0xFF);
}

esp_err_t save_file_txt(const char *response)
{
  sprintf(pathFile, "%s%s", mount_point, file_txt);
  FILE *file = fopen(pathFile, "w, ccs=UTF-8");
  if (file == NULL)
  {
    ESP_LOGE(TAG, "Failed to open file: %s: %s (errno: %d)", file_txt, strerror(errno), errno);
    return ESP_FAIL;
  }
  // Ghi dữ liệu vào file
  fprintf(file, "%s", response);
  fclose(file);
  printf("Saved file %s\n", file_txt);
  return ESP_OK;
}

const char* find_value_by_key_recursive(cJSON *node, const char *key) {
  if (node == NULL) return NULL;

  if (cJSON_IsObject(node)) {
      cJSON *child = node->child;
      while (child) {
          if (strcmp(child->string, key) == 0 && cJSON_IsString(child)) {
              return child->valuestring;
          }
          const char *found = find_value_by_key_recursive(child, key);
          if (found) return found;
          child = child->next;
      }
  }

  if (cJSON_IsArray(node)) {
      int size = cJSON_GetArraySize(node);
      for (int i = 0; i < size; i++) {
          const char *found = find_value_by_key_recursive(cJSON_GetArrayItem(node, i), key);
          if (found) return found;
      }
  }

  return NULL;
}

esp_err_t client_event_post_handler(esp_http_client_event_handle_t evt)
{
  switch (evt->event_id)
  {
  case HTTP_EVENT_ON_DATA:
    printf("SERVER_RESPONE:\n ==================== Transcription ====================\n");
    printf("%.*s", evt->data_len, (char *)evt->data);
    printf("\n====================      End      ====================\n");
    save_file_txt((char *)evt->data);
    xTaskCreate(&smtp_client_task, "smtp_client_task", 1024 * 10, NULL, 5, NULL);
    break;
  case HTTP_EVENT_ON_FINISH:
    ESP_LOGI(TAG, "HTTP request finished");
    break;
  default:
    break;
  }
  return ESP_OK;
}

static void upload()
{
 ESP_LOGI(TAG, "Stack Upload: %d", uxTaskGetStackHighWaterMark(NULL));

  sprintf(pathFile, "%s%s", mount_point, file_wav);

  FILE *file = fopen(pathFile, "rb");
  if (file == NULL)
  {
    ESP_LOGE(TAG, "Failed to open file %s: %s", pathFile, strerror(errno));
    return;
  }

  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);
  printf("Audio File found, size: %ld\n", file_size);
  printf("Uploading file to server...\n");

  esp_http_client_config_t config = {
      .url = "https://generativelanguage.googleapis.com/upload/v1beta/files?key=AIzaSyC8J0qXiwqLqOti3UUtrpapuDoP-i8LHTY",
      .method = HTTP_METHOD_POST,
      .transport_type = HTTP_TRANSPORT_OVER_SSL,
      .buffer_size = 8192,
      .timeout_ms = 50000,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);

  esp_http_client_set_header(client, "Content-Type", "audio/wav");
  char content_length_str[16];
  snprintf(content_length_str, sizeof(content_length_str), "%ld", file_size);
  esp_http_client_set_header(client, "Content-Length", content_length_str);

  esp_err_t err = esp_http_client_open(client, file_size);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
    fclose(file);
    esp_http_client_cleanup(client);
    return;
  }
  size_t buffer_size = 1024;
  char *buffer = malloc(buffer_size);
  size_t total_bytes_sent = 0;
  size_t bytes_read;

  while (total_bytes_sent < file_size)
  {
    bytes_read = fread(buffer, 1, buffer_size, file);
    if (bytes_read == 0)
      break;
    int bytes_written = esp_http_client_write(client, buffer, bytes_read);
    if (bytes_written < 0)
      break;
    total_bytes_sent += bytes_written;
    ESP_LOGI(TAG, "Sent %d/%ld bytes", total_bytes_sent, file_size);
  }
  free(buffer);
  fclose(file);

  esp_http_client_fetch_headers(client);
  int status_code = esp_http_client_get_status_code(client);
  ESP_LOGI(TAG, "HTTP POST Status = %d", status_code);

  char uri[128] = {0};
  char prompt[] =
    "Bạn đang nghe audio về cuộc học đồ án giữa sinh viên và giảng viên\n\n"
    "Việc của bạn là tóm tắt các việc sinh viên đã làm trong 2 tuần qua. Và kế hoạch trong 2 tuần tới (nếu sinh viên có đề cập)\n\n"
    "Viết thành các ý chính, không cần viết thành văn bản hoàn chỉnh\n\n";

  if (status_code == 200 || status_code == 201)
  {
    size_t size_reponse_uri = 512;
    char *response_uri = malloc(size_reponse_uri);
      int len = esp_http_client_read(client, response_uri, size_reponse_uri - 1);
      if (len > 0)
      {
          response_uri[len] = '\0';
          ESP_LOGI(TAG, "Upload Response: %s", response_uri);
  
          cJSON *root = cJSON_Parse(response_uri);
          if (root) {
              const char *uri_found = find_value_by_key_recursive(root, "uri");
              if (uri_found) {
                  strncpy(uri, uri_found, sizeof(uri) - 1);
                  uri[sizeof(uri) - 1] = '\0';
                  ESP_LOGI(TAG, "Extracted URI: %s", uri);
              } else {
                  ESP_LOGW(TAG, "URI not found in response");
              }
              cJSON_Delete(root);
          } else {
              ESP_LOGE(TAG, "Failed to parse JSON");
          }
      }
      free(response_uri);
  }
  else
  {
    ESP_LOGE(TAG, "Upload failed with status %d", status_code);
    esp_http_client_cleanup(client);
    return;
  }
  
  if (!uri[0])
  {
    ESP_LOGE(TAG, "Failed to extract URI from upload response");
    esp_http_client_cleanup(client);
    return;
  }
  esp_http_client_cleanup(client);
  // Step2: Send URI to generateContent
  esp_http_client_config_t generate_config = {
      .url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-1.5-flash:generateContent?key=AIzaSyC8J0qXiwqLqOti3UUtrpapuDoP-i8LHTY",
      .method = HTTP_METHOD_POST,
      .transport_type = HTTP_TRANSPORT_OVER_SSL,
      .buffer_size = 8192,
      .timeout_ms = 60000,
  };
  client = esp_http_client_init(&generate_config);

  char json_payload[1024];
  int written = snprintf(json_payload, sizeof(json_payload),
                         "{\"contents\": [{\"parts\": [{\"text\": \"%s\"}, {\"fileData\": {\"fileUri\": \"%s\", \"mimeType\": \"audio/wav\"}}]}], \"generationConfig\": {   \"temperature\": 1, \"top_p\": 0.95, \"top_k\": 40,\"maxOutputTokens\": 10000, \"response_mime_type\": \"text/plain\"}}",
                         prompt, uri);
  if (written < 0 || written >= sizeof(json_payload)) {
      ESP_LOGE(TAG, "JSON payload buffer overflow or error");
      esp_http_client_cleanup(client);
      return;
  }

  esp_http_client_set_header(client, "Content-Type", "application/json");

  err = esp_http_client_open(client, strlen(json_payload));
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to open HTTP connection for generateContent: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return;
  }

  int bytes_written = esp_http_client_write(client, json_payload, strlen(json_payload));
  if (bytes_written < 0)
  {
    ESP_LOGE(TAG, "Failed to write data for generateContent");
    esp_http_client_cleanup(client);
    return;
  }
  ESP_LOGI(TAG, "Wrote %d bytes for generateContent", bytes_written);

  esp_http_client_fetch_headers(client);
  int generate_status = esp_http_client_get_status_code(client);
  ESP_LOGI(TAG, "GenerateContent HTTP Status = %d", generate_status);
  if (status_code == 200 || status_code == 201)
  {
    size_t size_reponse_text = 1024;
      char *response_text = malloc(size_reponse_text);
      if (response_text == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for response");
        return;
    }
      int len = esp_http_client_read(client, response_text, size_reponse_text - 1);
      if (len > 0)
      {
          response_text[len] = '\0';
          ESP_LOGI(TAG, "Response from Gemini: %s", response_text);
  
          cJSON *root1 = cJSON_Parse(response_text);
          if (root1) {
              const char *uri_found = find_value_by_key_recursive(root1, "text");
              if (uri_found) {
                  strncpy(response_text, uri_found, size_reponse_text - 1);
                  response_text[size_reponse_text - 1] = '\0';
                  printf("SERVER_RESPONE: ==================== Transcription ====================\n");
                  printf("%s", response_text);
                  printf("SERVER_RESPONE: ==================== END ====================\n");
                  save_file_txt(response_text);
                 xTaskCreate(&smtp_client_task, "smtp_client_task", 1024 * 8, NULL, 5, NULL);
              } else {
                  ESP_LOGW(TAG, "URI not found in response");
              }
              cJSON_Delete(root1);
          } else {
              ESP_LOGE(TAG, "Failed to parse JSON");
          }
      }
      free(response_text);
  }

  esp_http_client_cleanup(client);
}

static void post_task(void *arg)
{
  upload();
  vTaskDelete(NULL);
}
static void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  switch (event_id)
  {
  case WIFI_EVENT_STA_START:
    printf("WiFi connecting ... \n");
    break;
  case WIFI_EVENT_STA_CONNECTED:
    printf("WiFi connected to ap SSID: %s password: %s\n", WIFI_SSID, WIFI_PASS);
    retry_count = 0;
    break;
  case WIFI_EVENT_STA_DISCONNECTED:
    printf("WiFi lost connection ... \n");
    if (retry_count < MAX_RETRY)
    {
      retry_count++;
      printf("Reconnecting to WiFi... Attempt %d/%d\n", retry_count, MAX_RETRY);
      esp_wifi_connect();
    }
    break;
  case IP_EVENT_STA_GOT_IP:
    printf("WiFi got IP ... \n\n");
    xTaskCreate(post_task, "post_task", 1024 * 15, NULL, 10, NULL);
    break;
  default:
    break;
  }
}

void wifi_connection()
{
  // 1 - Wi-Fi/LwIP Init Phase
  esp_netif_init();                    // TCP/IP initiation 					s1.1
  esp_event_loop_create_default();     // event loop 			                s1.2
  esp_netif_create_default_wifi_sta(); // WiFi station 	                    s1.3
  wifi_init_config_t wifi_initiation = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&wifi_initiation); // 					                    s1.4

  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
  wifi_config_t wifi_configuration = {
      .sta = {
          .ssid = WIFI_SSID,
          .password = WIFI_PASS}};
  esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_configuration);
  esp_wifi_start();
  esp_wifi_connect();
}

void app_main(void)
{
  recording_done_semaphore = xSemaphoreCreateBinary();
  if (recording_done_semaphore == NULL)
  {
      ESP_LOGE(TAG, "Failed to create semaphore");
      return;
  }

  spiInit();
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "SD card mount failed, cannot open file.");
    return;
  }

  sprintf(pathFile, "%s/%s", mount_point, file_wav);
  file = fopen(pathFile, "wb");
  if (file == NULL)
  {
    ESP_LOGE(TAG, "Failed to open file: %s", strerror(errno));
  }
  byte header[headerSize];
  wavHeader(header, RECORD_SIZE);
  fwrite(header, 1, headerSize, file);

  i2sInit();
  xTaskCreate(i2s_adc, "i2s_adc",1024 * 2, NULL, 8, NULL);
  ESP_LOGI(TAG, "Waiting for recording to finish...");
  xSemaphoreTake(recording_done_semaphore, portMAX_DELAY);
  ESP_LOGI(TAG, "Recording finished, starting WiFi connection...");

  nvs_flash_init();
  wifi_connection();S

  vSemaphoreDelete(recording_done_semaphore);
}
