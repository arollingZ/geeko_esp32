这是一个基于ESP32S3M-N16R8的功能演示demo工程，ESP-IDF版本为6.1.0，其主要功能如下：

1. 支持TF卡数据读写
	CS PIN:IO2
	MOSI PIN:IO11
	SCK PIN:IO12
	MISO PIN:IO13
	可以读写TF卡数据，并验证读写准确性
2. 支持WiFi配网
	(本设备默认WiFi名称为:ESP32_DEMO)手机连接到本设备的WiFi后，本设备开启WiFi scan，扫描附近的热点，手机访问浏览器任意网页，进入配网界面，
	配网界面显示出本设备扫描到的WiFi热点，选择指定热点，并填入密码，即可配网成功，本设备要将WiFi热点名称和密码存储到TF卡，发生重启后可自动恢复网络
	配网成功后，设备验证网络，并实时监控WiFi连接状态，一旦WiFi中断，则开启WiFi scan并进行重连，此时也可以连接本设备WiFi并重新配网
3. LED指示灯
	pin脚：IO1
	用于指示网络状态
	a. 在配网未成功状态下，LED灯熄灭
	b. 配网成功，但是未扫描到指定热点，快闪2下(快闪间隔100ms,即亮100ms，灭100ms,再亮100ms，再熄灭)，然后熄灭1秒，循环执行
	c. 配网成功，并且扫描并连接到指定热点，但是网络不通时，慢闪(亮500ms,灭500ms)，循环执行
	d. 网络通了以后，LED保持常亮
4. LCD显示功能
	CS PIN:IO39
	BL PIN:IO41
	DC PIN:IO40
	RST PIN:IO38
	LCD驱动芯片型号：ST7735S, 0.96 inch 80RGB*160DOTS
5. 麦克风拾音及播放功能
	音频 		codec	ES8388（24bit，ADC + DAC 二合一，I2C 控制）
	I2S 引脚	BCLK=GPIO7，WS=GPIO16，DIN=GPIO17，DOUT=GPIO15，MCLK=GPIO6
	I2C 引脚	SDA=GPIO4，SCL=GPIO5
	采样率		输入 24 kHz / 输出 24 kHz
	功放使能	GPIO42（低电平开启），插耳机检测脚 GPIO3
	显示		160×80 SPI 屏（ST7789），背光 GPIO41
	按键		BOOT=GPIO0（长按>300MS开始录音、松开停止，短按<300MS播放录音，如没有录音记录，则LCD提示），音量加 GPIO48 / 减 GPIO47
6. 录音数据

2026/11/11
以上基本功能均已实现

