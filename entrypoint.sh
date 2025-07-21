#!/bin/bash
#set -ex

dlt-daemon > /tmp/log/dlt-daemon.log 2>&1 &
routingmanagerd > /tmp/log/routingmanagerd.log 2>&1 &

engine-service > /tmp/log/engine.log 2>&1 &
radio-service > /tmp/log/radio.log 2>&1 &

#/radio-client
socat TCP-LISTEN:8000,reuseaddr,fork EXEC:radio-client,pty > /tmp/log/socat.log 2>&1

# socat -,raw,echo=0 TCP-CONNECT:127.0.0.1:8000
#bash
