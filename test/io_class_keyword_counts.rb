# A keyword hash a class method takes is counted apart from its positionals,
# as CRuby counts it; one a method does not take counts as a positional.
require "tmpdir"
src = File.join(Dir.tmpdir, "spinel_io_class_kw_counts_src.txt")
File.write(src, "abc\n")
f = File.open(src)
fd = f.fileno
def t; yield; rescue => e; p [e.class, e.message]; end
h = {}
# a keyword hash the method takes is counted apart: CRuby's message gives
# the positionals alone
t { IO.for_fd(fd, "r", {}, autoclose: false) }
t { IO.new(fd, "r", 0, autoclose: false) }
t { File.for_fd(fd, "r", {}, autoclose: false) }
t { File.pipe(1, 2, 3, x: 1) }
# and so is a `**` operand, which adds no positional to such a method
t { IO.for_fd(fd, "r", 0, **h) }
t { IO.new(fd, "r", 0, **{}) }
def built
  puts "built"
  {}
end
t { IO.for_fd(fd, "r", 0, **built) }
def forward(fd, **) = IO.for_fd(fd, "r", 0, **)
t { forward(fd, autoclose: false) }
# a positional hash still counts
t { IO.new(fd, "r", {}) }
t { IO.new(fd, "r", 1) }
t { IO.new(fd, "r", h) }
t { p IO.new(fd, "r", autoclose: false).fileno == fd }
t { p IO.for_fd(fd, autoclose: false).fileno == fd }
io = IO.new(fd, "r", **{})
io.autoclose = false
p io.fileno == fd
# a method that takes no keywords counts the hash as a positional
t { IO.sysopen(src, 0, 0644, x: 1) }
t { IO.select([f], nil, nil, 0, x: 1) }
f.close
File.delete(src)
