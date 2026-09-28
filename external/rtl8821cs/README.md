### 第一步，安装编译工具
```
mkdir -p /usr/local/toolchain  
cd /usr/local/toolchain
```
#### 下载编译工具
```
wget https://github.com/ophub/kernel/releases/download/dev/arm-gnu-toolchain-13.3.rel1-aarch64-aarch64-none-elf.tar.xz
```
#### 解压
```
tar -Jxf arm-gnu-toolchain-13.3.rel1-aarch64-aarch64-none-elf.tar.xz
```
### 第二步，下载驱动，编译
#### 下载驱动源码
```
cd ~/  
git clone https://github.com/u-osmi/rtl8821cs-arm64.git  
cd ~/rtl8821cs-arm64
```
#### 编译驱动
```
make
```

### 第三步，安装驱动
```
sudo mkdir /lib/modules/$(uname -r)/kernel/drivers/net/wireless/rtl8821cs  
sudo cp -f rtl8821cs.ko /lib/modules/$(uname -r)/kernel/drivers/net/wireless/rtl8821cs
```
#### 更新模块依赖关系
```
sudo depmod -a
```
#### 加载驱动模块
```
sudo modprobe rtl8821cs
```
#### 检查驱动是否加载成功
```
lsmod | grep rtl8821cs
```
#### 可以看到成功加载驱动  
```
rtl8821cs               1843200  0  
cfg80211                917504  2 8822cs,brcmfmac
```
