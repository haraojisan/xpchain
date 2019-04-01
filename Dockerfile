FROM ubuntu:bionic AS build

# Change APT source (Development purpose only)
# RUN \
#   sed -i".bak" \
#     "s@http://archive\.ubuntu\.com@http://ubuntu-ashisuto\.ubuntulinux\.jp/ubuntu@" \
#     /etc/apt/sources.list

# Basic settings
RUN \
  apt-get update && \
  DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    apt-utils && \
  DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    language-pack-en tzdata software-properties-common && \
  apt-get clean && \
  rm -rf /var/lib/apt/lists/* && \
  update-locale LANG=en_US.UTF-8

# Prepare for build xpchaind
RUN \
  apt-add-repository ppa:bitcoin/bitcoin -y && \
  apt-get update && \
  DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential libtool autotools-dev automake pkg-config libssl-dev \
    libevent-dev bsdmainutils python3 libboost-system-dev \
    libboost-filesystem-dev libboost-chrono-dev libboost-program-options-dev \
    libboost-test-dev libboost-thread-dev libdb4.8-dev libdb4.8++-dev \
    libminiupnpc-dev libzmq3-dev libunivalue-dev ca-certificates curl && \
  apt-get clean && \
  rm -rf /var/lib/apt/lists/*

# Build XPChaind
COPY . /usr/src/xpchain
WORKDIR /usr/src/xpchain
RUN \
  ./autogen.sh && \
  ./configure --without-gui --disable-tests --disable-bench && \
  make -j$(nproc) && \
  make install && \
  make clean


FROM ubuntu:bionic

# Change APT source (Development purpose only)
# RUN \
#   sed -i".bak" \
#     "s@http://archive\.ubuntu\.com@http://ubuntu-ashisuto\.ubuntulinux\.jp/ubuntu@" \
#     /etc/apt/sources.list

ENV \
  XPCHAIND_DATA_DIR=/home/wallet/.xpchain

# Basic settings
RUN \
  apt-get update && \
  DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    apt-utils && \
  DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    language-pack-en tzdata software-properties-common gosu && \
  apt-get clean && \
  rm -rf /var/lib/apt/lists/* && \
  update-locale LANG=en_US.UTF-8

# Create a normal user
RUN \
  useradd -d /home/wallet -m -s /bin/bash wallet && \
  gosu wallet mkdir ${XPCHAIND_DATA_DIR}

# Prepare for run xpchaind
RUN \
  apt-add-repository ppa:bitcoin/bitcoin -y && \
  apt-get update && \
  DEBIAN_FRONTEND=noninteractive apt-get install -y \
    libboost-chrono1.65.1 libboost-filesystem1.65.1 libboost-system1.65.1 \
    libboost-test1.65.1 libboost-thread1.65.1 libc6 libdb4.8++ libevent-2.1-6 \
    libevent-pthreads-2.1-6 libgcc1 libminiupnpc10 libnorm1 libpgm-5.2-0 \
    libsodium23 libssl1.1 libstdc++6 libzmq5 && \
  apt-get clean && \
  rm -rf /var/lib/apt/lists/*

# Copy build xpchaind (and other tools)
COPY --from=build \
  /usr/local/bin/xpchain-cli /usr/local/bin/xpchain-tx /usr/local/bin/xpchaind \
  /usr/local/bin/

# Install debug tools (Development purpose only)
# RUN \
#   apt-get update && \
#   DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
#     vim sudo && \
#   apt-get clean && \
#   rm -rf /var/lib/apt/lists/* && \
#   echo "wallet ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/wallet

# Place an entrypoint script (No need at this point)
# COPY docker-entrypoint.sh /
# RUN \
#   chmod +x /docker-entrypoint.sh && \

USER wallet
WORKDIR /home/wallet
VOLUME ["${XPCHAIND_DATA_DIR}"]
EXPOSE 8762 8798 18762 18798
# ENTRYPOINT ["/docker-entrypoint.sh"]
CMD ["/usr/local/bin/xpchaind", "-printtoconsole", "-nodebuglogfile"]
