# ADI free GNU bfin-elf toolchain (2014R1-RC2, GCC 4.3), x86_64 only.
FROM --platform=linux/amd64 debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl bzip2 make python3 \
 && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL -o /tmp/tc.tar.bz2 \
  "https://downloads.sourceforge.net/project/adi-toolchain/2014R1/2014R1-RC2/x86_64/blackfin-toolchain-elf-gcc-4.3-2014R1-RC2.x86_64.tar.bz2" \
 && tar -xjf /tmp/tc.tar.bz2 -C / && rm /tmp/tc.tar.bz2
ENV PATH="/opt/uClinux/bfin-elf/bin:${PATH}"
WORKDIR /work
