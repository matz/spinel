# IO#each_line reads its separator, limit and chomp: when a splat or a
# double splat forwards them, as it does when they are written out: the
# forwarded forms dropped all three and split on "\n".
require "tmpdir"
path = File.join(Dir.tmpdir, "spinel_each_line_splat_#{Process.pid}")
File.write(path, "one\ntwo\r\n\nthree;four\nfive")

def run(path, *a, **k)
  out = []
  File.open(path) { |f| f.each_line(*a, **k) { |l| out << l } }
  out
end
p run(path)
p run(path, chomp: true)
p run(path, ";")
p run(path, 3)
p run(path, "o", 2)
p run(path, "o", 2, chomp: true)
p run(path, "")
p run(path, nil)
p run(path, nil, 4)
p run(path, ";", nil)

def anon(path, *, **)
  out = []
  File.open(path) { |f| f.each_line(*, **) { |l| out << l } }
  out
end
p anon(path, "o", chomp: true)
p anon(path, 4)

def lines(path)
  out = []
  File.open(path) { |f| yield f, out }
  out
end
a = [";"]
k = {chomp: true}
p lines(path) { |f, o| f.each_line(*a, chomp: true) { |l| o << l } }
p lines(path) { |f, o| f.each_line(";", *[2]) { |l| o << l } }
p lines(path) { |f, o| f.each_line(**k) { |l| o << l } }
p lines(path) { |f, o| f.each_line(**k, chomp: false) { |l| o << l } }
p lines(path) { |f, o| f.each_line(chomp: false, **k) { |l| o << l } }
p lines(path) { |f, o| f.each(*a, **k) { |l| o << l } }

[[1, 2, 3], [0], [:x], [nil, "a"], ["o", "x"]].each do |args|
  begin
    p lines(path) { |f, o| f.each_line(*args) { |l| o << l } }
  rescue ArgumentError, TypeError => e
    p [e.class, e.message]
  end
end

# a limit of 0 written out raises as the spread one does
zero = 0
[-> { lines(path) { |f, o| f.each_line(0) { |l| o << l } } },
 -> { lines(path) { |f, o| f.each_line(zero) { |l| o << l } } },
 -> { lines(path) { |f, o| f.each_line("o", 0) { |l| o << l } } },
 -> { lines(path) { |f, o| f.each_line(nil, 0, chomp: true) { |l| o << l } } },
 -> { lines(path) { |f, o| f.each(*[0]) { |l| o << l } } },
 -> { lines(path) { |f, o| f.each_line(-1) { |l| o << l } } }].each do |call|
  begin
    p call.call
  rescue ArgumentError => e
    p [e.class, e.message]
  end
end
File.delete(path)
