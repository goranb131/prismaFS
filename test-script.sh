#!/bin/sh
# test.sh - basic tests for PrismaFS

BASE=$(mktemp -d)
SESSION=$(mktemp -d)
MNT=$(mktemp -d)
echo "base $BASE" > /tmp/test.conf
echo "session $SESSION" >> /tmp/test.conf

echo "hello" > $BASE/testfile.txt

./prismafs -c /tmp/test.conf $MNT
sleep 0.5

# tests
ls $MNT | grep testfile.txt || echo "FAIL: readdir"
cat $MNT/testfile.txt | grep hello || echo "FAIL: read"
cat $MNT/dev/cpu || echo "FAIL: dev/cpu"

umount $MNT
rm -rf $BASE $SESSION $MNT /tmp/test.conf
echo "done"