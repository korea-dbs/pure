# storage_fullpage, built end-to-end from vanilla MySQL 8.0.24 + sysbench
# already installed and wired up for scripts/crash_microbench.sh.
#
# Build from the REPO ROOT (not this directory), since it needs to COPY
# storage_fullpage/storage/innobase and scripts/crash_microbench.sh:
#
#   docker build -f dockerimage/Dockerfile -t ofr-fullpage .
#   docker run -it --rm ofr-fullpage
#
# Note: there is no real NVDIMM/DAX inside a container. /mnt/pmem is just a
# regular directory here, so OFR's three backing files land on the
# container's overlay filesystem instead of real persistent memory. The
# crash-recovery *logic* is unaffected (fallocate + mmap work the same way
# on any filesystem) -- you only lose the NVDIMM latency/durability
# characteristics. To get a real DAX mount, bind-mount a host DAX device at
# /mnt/pmem with `docker run -v /mnt/pmem:/mnt/pmem ...` on a host that has
# one set up (see the top-level README's NVDIMM section).

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake bison pkg-config git wget ca-certificates \
        libssl-dev libncurses5-dev zlib1g-dev libnuma-dev libaio-dev \
        libudev-dev libtirpc-dev liburing-dev \
        python3 perl bc procps \
        mysql-client sysbench \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt

# 1. Vanilla MySQL 8.0.24 source (bundled boost included in this tarball)
RUN wget -q https://downloads.mysql.com/archives/get/p/23/file/mysql-boost-8.0.24.tar.gz \
    && tar xzf mysql-boost-8.0.24.tar.gz \
    && rm mysql-boost-8.0.24.tar.gz \
    && mv mysql-8.0.24 mysql-src

# 2. Swap in the OFR fullpage engine
RUN rm -rf /opt/mysql-src/storage/innobase
COPY storage_fullpage/storage/innobase /opt/mysql-src/storage/innobase

# 3. Configure + build (this takes a while and needs a few GB of RAM)
RUN mkdir /opt/mysql-src/build
WORKDIR /opt/mysql-src/build
RUN cmake .. -DCMAKE_BUILD_TYPE=Release \
             -DWITH_BOOST=/opt/mysql-src/boost \
             -DDOWNLOAD_BOOST=OFF \
    && cmake --build . --target mysqld -j "$(nproc)" \
    && cmake --build . --target mysql -j "$(nproc)"

ENV MYSQLD_BIN=/opt/mysql-src/build/runtime_output_directory/mysqld
ENV DEFAULTS_FILE=/etc/mysql/my.cnf
ENV ERROR_LOG=/var/lib/mysql-data/mysql_error.log
ENV SOCK=/tmp/mysql.sock
ENV PATH="/opt/mysql-src/build/runtime_output_directory:${PATH}"

RUN mkdir -p /var/lib/mysql-data /mnt/pmem /etc/mysql

COPY dockerimage/my.cnf /etc/mysql/my.cnf
COPY dockerimage/entrypoint.sh /entrypoint.sh
COPY scripts/crash_microbench.sh /scripts/crash_microbench.sh
RUN chmod +x /entrypoint.sh /scripts/crash_microbench.sh

WORKDIR /
ENTRYPOINT ["/entrypoint.sh"]
CMD ["/bin/bash"]
