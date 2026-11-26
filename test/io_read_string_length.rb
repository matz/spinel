# File.read and IO.read take an Integer length and offset: a String there is
# a TypeError, and a wrong path is named first. CRuby converts the offset
# before it opens the file and the length after, so a missing file is named
# ahead of a wrong length and behind a wrong offset. An IO-or-StringIO receiver
# read from a Hash keeps its class arm for IO.read (rack's multipart parser).
require "stringio"
require "tmpdir"

path = File.join(Dir.tmpdir, "spinel_io_read_string_length_#{Process.pid}.txt")
File.write(path, "abcdef")

def try
  p yield
rescue TypeError, ArgumentError, SystemCallError => e
  p [e.class, e.message]
end

try { File.read(path, "x") }
try { IO.read(path, "x") }
try { File.read(path, 2, "x") }
try { File.read(path, :two) }
try { File.read(path, [2]) }
try { IO.read(4, String.new) }
try { File.read("/nope/x", "x") }
try { IO.read("/nope/x", :a) }
try { File.read("/nope/x", 2, "x") }
try { File.read("/nope/x", "x", 1) }
try { File.read(path, "x", 1) }
try { File.read(path, "x", -1) }
try { File.read("/nope/x", "x", -1) }
try { File.binread(path, "x") }
try { File.binread("/nope/x", "x") }
try { File.binread("/nope/x", "x", "y") }
begin; File.binread(path, "x", "y"); rescue TypeError => e; p e.class; end
begin
  File.binread(path, "x", -1)
rescue SystemCallError => e
  p e.class
end
n = ARGV.empty? ? "2" : 2
try { File.read(path, n) }
try { File.read(path, 2) }
try { File.read(path, nil, 1) }
# a boxed length: a Float converts (the read opens the file once), nil reads the rest
m = [2.9, "x", nil][ARGV.size]
try { File.read(path, m) }
try { File.read(path, [2.9, "x", nil][2]) }

def read_data(io, n) = io.read(n, String.new)
env = { "in" => StringIO.new("abcdef") }
env["tmp"] = [[1]]
p read_data(env["in"], env["n"] || 4)
File.delete(path)
