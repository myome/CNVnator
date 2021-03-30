FROM bitnami/minideb:buster as builder

WORKDIR /root

RUN apt update && apt install -y --no-install-recommends \
  ca-certificates \
  g++ \
  bzip2 \
  git \
  autoconf \
  libx11-dev \
  libxpm-dev \
  libxft-dev \
  libxext-dev \
  libssl-dev \
  automake \
  cmake \
  libbz2-dev \
  libcurl3-dev \
  libfreetype6-dev \
  liblzma-dev \
  libncurses5-dev \
  libreadline-dev \
  libpython2.7 \
  libz-dev \
  make \
  python-matplotlib \
  python-scipy \
  python-tk \
  curl \
 && rm -rf /var/lib/apt/lists/*

RUN mkdir install_root

RUN git clone --branch v6-22-00-patches https://github.com/root-project/root.git root_src && \
  mkdir root_src/build_dir && \
  cd root_src/build_dir && \
  cmake -DCMAKE_INSTALL_PREFIX=/root/install_root ../ && \
  cmake --build . --target install -- -j $(nproc)

RUN cd /root

RUN git clone --recursive  https://github.com/samtools/htslib.git htslib && \
  cd ./htslib && \
  autoreconf -i && \
  ./configure --prefix=/root/install_root && \
  make -j $(nprocs) && \
  make install

RUN git clone https://github.com/jvanalstine/samtools.git samtools && \
  cd ./samtools && \
  git checkout feature/library_support && \
  autoheader && \
  autoconf -Wno-syntax && \
  ./configure --prefix=/root/install_root && \
  make -j $(nprocs) && \
  make install



RUN echo "/root/install_root/lib" >> /etc/ld.so.conf.d/root.conf && \
  ldconfig

COPY ./ /root/cnvnator
RUN cd /root/cnvnator && \
  INSTALL_PREFIX=/root/install_root make -j $(nproc) && \
  cp ./cnvnator *.py *.pl /root/install_root/bin && \
  cd /root

FROM bitnami/minideb:buster
COPY --from=builder /root/install_root/lib /usr/local/lib
COPY --from=builder /root/install_root/include /usr/local/include
COPY --from=builder /root/install_root/bin /usr/local/bin

RUN apt update && apt install -y --no-install-recommends \
  libreadline-dev \
  libcurl3-dev \
  libssl-dev \
  libgomp1 \
  libfreetype6-dev \
  && rm -rf /var/lib/apt/lists/*

RUN ldconfig