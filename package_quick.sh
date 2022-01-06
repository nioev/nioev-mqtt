#!/bin/bash
cd "$(dirname "$0")"

cp thirdparty/quickjs-2021-03-27/quickjs.h .
xxd -i quickjs.h $1/quickjs-carr.h
rm quickjs.h
