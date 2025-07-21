FROM --platform=linux/arm64 quay.io/rhadp-examples/application:latest

COPY . /opt/src

RUN cmake -S /opt/src -B /opt/build && \
  make -C /opt/build install && \
  rm -rf /opt/src && \
  chown 1001:0 /usr/local/bin/*

RUN ls -la /opt/build
RUN ls -la /usr/local/bin

# missing dirs
RUN mkdir -p /tmp/log && mkdir -p /run/dlt && mkdir -p /run/vsomeip

# permissions & ownership
RUN chown 1001:1001 /tmp/log && chmod -R 777 /tmp/log && \
  chown 1001:0 /run/dlt && chmod -R 777 /run/dlt && \
  chown 1001:0 /run/vsomeip && chmod -R 777 /run/vsomeip

COPY entrypoint.sh /usr/local/bin/

RUN chown 1001:0 /usr/local/bin/*

USER 1001

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]