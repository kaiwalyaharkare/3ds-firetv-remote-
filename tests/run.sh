#!/usr/bin/env bash
# Builds adb.c for your PC and runs it against a fake Fire TV.
# Needs: gcc, python3 + `pip install cryptography`, mbedtls 2.28 (libmbedtls-dev).
# Optional: MBEDTLS=/path/to/mbedtls-source-tree
set -e
cd "$(dirname "$0")"
INC=""; LIB=""
if [ -n "$MBEDTLS" ]; then INC="-I$MBEDTLS/include"; LIB="-L$MBEDTLS/library"; fi
gcc -Wall -Wextra -O2 $INC host_test.c ../3ds/source/adb.c $LIB -lmbedtls -lmbedx509 -lmbedcrypto -o /tmp/firetv_host_test
rm -f /tmp/firetv_mock.log /tmp/firetv_test_key
python3 mock_adbd.py 127.0.0.77 5557 /tmp/firetv_mock.log & MOCK=$!
trap 'kill $MOCK 2>/dev/null' EXIT
sleep 1
/tmp/firetv_host_test /tmp/firetv_test_key
sleep 0.3
echo "=== what the fake TV received ==="
cat /tmp/firetv_mock.log
! grep -q "ASSERT FAIL" /tmp/firetv_mock.log && echo "PASS"
