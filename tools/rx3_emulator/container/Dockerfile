# SPDX-License-Identifier: MPL-2.0

FROM arm32v7/debian:bullseye-slim

RUN apt-get update -qq \
 && apt-get install -y -qq --no-install-recommends clang gcc libc6-dev libasound2-dev lld \
 && rm -rf /var/lib/apt/lists/*

COPY fbshim.c /opt/rx3emu/fbshim.c
COPY audioshim.c /opt/rx3emu/audioshim.c
COPY audioshim.map /opt/rx3emu/audioshim.map
COPY run.sh /opt/rx3emu/run.sh

RUN gcc -shared -fPIC -O2 -Wall -Wextra \
      -o /opt/rx3emu/fbshim.so /opt/rx3emu/fbshim.c -ldl \
 && gcc -shared -fPIC -O2 -Wall -Wextra \
      -Wl,--version-script=/opt/rx3emu/audioshim.map \
      -o /opt/rx3emu/audioshim.so /opt/rx3emu/audioshim.c \
 && chmod 755 /opt/rx3emu/run.sh

ENTRYPOINT ["/opt/rx3emu/run.sh"]
