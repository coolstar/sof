

# DTS codec setup config
define(`CA_SETUP_CONTROLBYTES',
``      bytes "0x53,0x4f,0x46,0x00,0x00,0x00,0x00,0x00,'
`       0x14,0x00,0x00,0x00,0x00,0x10,0x00,0x03,'
`       0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,'
`       0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,'
`       0x00,0x01,0x41,0x57,0x00,0x00,0x00,0x00,'
`       0x80,0xbb,0x00,0x00,0x20,0x00,0x00,0x00,'
`       0x02,0x00,0x00,0x00"''
)
define(`CA_SETUP_CONTROLBYTES_MAX', 8192)
define(`CA_SETUP_CONTROLBYTES_NAME', `DTS Codec Setup ')

define(`CA_RUNTIME_CONTROLBYTES',
``	bytes "0x53,0x4f,0x46,0x00,0x01,0x00,0x00,0x00,'
`	0x00,0x00,0x00,0x00,0x00,0x10,0x00,0x03,'
`	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,'
`	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00"''
)
define(`CA_RUNTIME_CONTROLBYTES_MAX', 8192)
define(`CA_RUNTIME_CONTROLBYTES_NAME', `DTS Codec Runtime ')

define(`CA_SCHEDULE_CORE', 0)

DECLARE_SOF_RT_UUID("DTS codec", dts_uuid, 0xd95fc34f, 0x370f, 0x4ac7, 0xbc, 0x86, 0xbf, 0xdc, 0x5b, 0xe2, 0x41, 0xe6)
define(`CA_UUID', dts_uuid)


include(`codec_adapter.m4')


define(CA_SETUP_CONFIG, concat(`ca_setup_config_', PIPELINE_ID))
define(CA_SETUP_CONTROLBYTES_NAME_PIPE, concat(CA_SETUP_CONTROLBYTES_NAME, PIPELINE_ID))


# Codec adapter setup config
CONTROLBYTES_PRIV(CA_SETUP_CONFIG, CA_SETUP_CONTROLBYTES)

# Codec adapter Bytes control for setup config
C_CONTROLBYTES(CA_SETUP_CONTROLBYTES_NAME_PIPE, PIPELINE_ID,
	CONTROLBYTES_OPS(bytes),
	CONTROLBYTES_EXTOPS(void, 258, 258),
	, , ,
	CONTROLBYTES_MAX(void, CA_SETUP_CONTROLBYTES_MAX),
	,
	CA_SETUP_CONFIG)

define(CA_RUNTIME_PARAMS, concat(`ca_runtime_params_', PIPELINE_ID))
define(CA_RUNTIME_CONTROLBYTES_NAME_PIPE, concat(CA_RUNTIME_CONTROLBYTES_NAME, PIPELINE_ID))

# Codec adapter runtime params
CONTROLBYTES_PRIV(CA_RUNTIME_PARAMS, CA_RUNTIME_CONTROLBYTES)

# Codec adapter Bytes control for runtime config
C_CONTROLBYTES(CA_RUNTIME_CONTROLBYTES_NAME_PIPE, PIPELINE_ID,
        CONTROLBYTES_OPS(bytes),
        CONTROLBYTES_EXTOPS(void, 258, 258),
        , , ,
        CONTROLBYTES_MAX(void, CA_RUNTIME_CONTROLBYTES_MAX),
        ,
        CA_RUNTIME_PARAMS)

ifdef(`CODEC_CONFIG_MODE_SEL', `

# Codec adapter enum control for config mode selection
define(`CA_CONFIG_MODE_SEL_NAME_PIPE', concat(`DTS Codec Config Mode ', PIPELINE_ID))

define(`CONTROL_NAME', `CA_CONFIG_MODE_SEL_NAME_PIPE')
# Codec adapter enum list. 0: bypass (optional), 1: speaker, 2: headphone.
CONTROLENUM_LIST(`CA_CONFIG_MODE_VALUES',
	LIST(`   ', `"bypass"', `"speaker"', `"headphone"'))

# Codec adapter enm control
C_CONTROLENUM(CA_CONFIG_MODE_SEL_NAME_PIPE, PIPELINE_ID,
	CA_CONFIG_MODE_VALUES,
	LIST(`  ', ENUM_CHANNEL(FC, 3, 0)),
	CONTROLENUM_OPS(enum, 257 binds the mixer control to enum get/put handlers, 257, 257))
undefine(`CONTROL_NAME')
')
