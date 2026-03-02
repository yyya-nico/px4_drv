// ptx_ioctl.h

#ifndef __PTX_IOCTL_H__
#define __PTX_IOCTL_H__

#include <linux/types.h>

// common definitions

enum ptx_system_type {
	PTX_UNSPECIFIED_SYSTEM = 0x00000000,
	PTX_ISDB_T_SYSTEM = 0x00000010,
	PTX_ISDB_S_SYSTEM = 0x00000020
};

enum ptx_stream_type {
	PTX_UNSPECIFIED_STREAM = 0x00000000,
	PTX_MPEG_TRANSPORT_STREAM = 0x00000010		// MPEG2-TS
};

// basic ioctls

struct ptx_freq {
	int freq_no;
	int slot;
};

#define PTX_SET_CHANNEL		_IOW(0x8d, 0x01, struct ptx_freq)
#define PTX_START_STREAMING	_IO(0x8d, 0x02)
#define PTX_STOP_STREAMING	_IO(0x8d, 0x03)
#define PTX_GET_CNR		_IOR(0x8d, 0x04, int *)
#define PTX_ENABLE_LNB_POWER	_IOW(0x8d, 0x05, int)
#define PTX_DISABLE_LNB_POWER	_IO(0x8d, 0x06)
#define PTX_SET_SYSTEM_MODE	_IOW(0x8d, 0x0b, int)

// extended ioctls

struct ptxt_cap {
	enum ptx_system_type systems;
	enum ptx_stream_type streams;
};

struct ptxt_info {
	char name[64];
	struct ptxt_cap cap;			// device capability information
};

enum ptxt_param_code {
	PTXT_UNDEFINED_PARAM = 0,
	PTXT_BANDWIDTH_PARAM = 1,
	PTXT_STREAM_ID_PARAM = 16
};

struct ptxt_additional_param {
	enum ptxt_param_code prop;
	__u32 data;
};

struct ptxt_params {
	enum ptx_system_type system;
	__u32 freq;				// ISDB-T: Hz, ISDB-S/S3: kHz
	__u32 num_prop;
	struct ptxt_additional_param *prop;
};

enum ptxt_stat_code {
	PTXT_UNKNOWN_STAT = 0,
	PTXT_SIGNAL_STRENGTH_STAT,
	PTXT_CNR_STAT
};

struct ptxt_stat {
	enum ptxt_stat_code stat;
	__u32 value;
};

struct ptxt_stats {
	__u32 num_stat;
	struct ptxt_stat *stat;
};

#define PTXT_GET_INFO		_IOR(0xe7, 0x00, struct ptxt_info *)
#define PTXT_GET_PARAMS		_IOR(0xe7, 0x01, struct ptxt_params *)
#define PTXT_SET_PARAMS		_IOW(0xe7, 0x02, struct ptxt_params *)
#define PTXT_CLEAR_PARAMS	_IO(0xe7, 0x03)
#define PTXT_TUNE		_IO(0xe7, 0x04)
#define PTXT_SET_LNB_VOLTAGE	_IOW(0xe7, 0x05, int)
#define PTXT_SET_CAPTURE	_IOW(0xe7, 0x06, bool)
#define PTXT_READ_STATS		_IOR(0xe7, 0x07, struct ptxt_stats *)

// B-CAS Smart Card ioctls

enum ptx_bcas_baudrate {
	PTX_BCAS_BAUDRATE_9600 = 0,
	PTX_BCAS_BAUDRATE_19200 = 1,
	PTX_BCAS_BAUDRATE_38400 = 2,
	PTX_BCAS_BAUDRATE_57600 = 3,
	PTX_BCAS_BAUDRATE_115200 = 4
};

struct ptx_bcas_data {
	__u8 *buf;
	__u8 len;
};

struct ptx_bcas_detect_card {
	__u8 detected;
};

struct ptx_bcas_set_baudrate {
	enum ptx_bcas_baudrate baudrate;
};

#define PTX_BCAS_DETECT_CARD	_IOR(0xe8, 0x01, struct ptx_bcas_detect_card)
#define PTX_BCAS_RESET_CARD	_IO(0xe8, 0x02)
#define PTX_BCAS_SEND_DATA	_IOW(0xe8, 0x03, struct ptx_bcas_data)
#define PTX_BCAS_RCV_DATA	_IOR(0xe8, 0x04, struct ptx_bcas_data)
#define PTX_BCAS_SET_BAUDRATE	_IOW(0xe8, 0x05, struct ptx_bcas_set_baudrate)

#endif
