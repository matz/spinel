#!/bin/sh
# A Base with N subclasses and 200 methods that each call two methods on an
# argument that may be any of them, beside 50 methods on Object: each call
# that enforces visibility asks the face of its entry once for each class
# it may land on (ct_face_for). Only Object's versions are told apart
# there; asking Object's method_missing and the top-level methods for every
# other class made the work grow with calls times subclasses (#11746).
N=${1:-10}
awk -v N="$N" 'BEGIN {
  print "class Object"
  for (i = 0; i < 50; i++) printf "  def ext%d = %d\n", i, i
  print "end"
  print "class Base"
  print "  def name = \"base\""
  print "  def size = 1"
  print "end"
  for (i = 0; i < N; i++) printf "class Sub%d < Base\n  def size = %d\nend\n", i, i
  print "class Runner"
  for (i = 0; i < 200; i++) printf "  def r%d(b) = b.size + b.name.size\n", i
  print "end"
  print "run = Runner.new"
  printf "all = [Base.new"
  for (i = 0; i < N; i++) printf ", Sub%d.new", i
  print "]"
  print "s = 0"
  print "all.each do |b|"
  for (i = 0; i < 200; i++) printf "  s += run.r%d(b)\n", i
  print "end"
  print "p s"
}'
