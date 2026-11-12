# A `**` operand passed to a class method that takes no keywords is one more
# positional only when the hash it spreads is not empty, as CRuby passes it:
# an empty one leaves the count alone, a full one is counted and checked.
require "tmpdir"
src = File.join(Dir.tmpdir, "spinel_class_method_double_splat_count.txt")
File.write(src, "abc\n")
def t(l)
  r = yield
  puts "#{l}: #{r.inspect}"
rescue => e
  puts "#{l}: #{e.class}: #{e.message}"
end
e = {}
f = {a: 1}
n = nil
# literals that spread nothing, or a key
t("empty literal") { IO.sysopen(src, 0, 0644, **{}).is_a?(Integer) }
t("nil") { IO.sysopen(src, 0, 0644, **nil).is_a?(Integer) }
t("nested empty") { IO.sysopen(src, 0, **{**{}}).is_a?(Integer) }
t("empty, too many") { IO.sysopen(src, 0, 0644, 1, **{}) }
t("full literal") { IO.sysopen(src, 0, 0644, **{a: 1}) }
t("pair and empty") { IO.sysopen(src, 0, 0644, a: 1, **{}) }
# a variable: the run time decides
t("empty variable") { IO.sysopen(src, 0, 0644, **e).is_a?(Integer) }
t("full variable") { IO.sysopen(src, 0, 0644, **f) }
t("nil variable") { IO.sysopen(src, 0, 0644, **n).is_a?(Integer) }
t("short, empty") { IO.sysopen(src, **e).is_a?(Integer) }
t("two operands") { IO.sysopen(src, 0, 0644, **e, **f) }
t("nested variable") { IO.sysopen(src, 0, 0644, **{**e}).is_a?(Integer) }
3.times { |i| t("loop #{i}") { IO.sysopen(src, 0, 0644, **(i.odd? ? f : e)).is_a?(Integer) } }
t("File") { File.sysopen(src, 0, 0644, **e).is_a?(Integer) }
t("File, full") { File.sysopen(src, 0, 0644, **f) }
# the arguments run once, in order, before the count is checked
def path(p)
  puts "path"
  p
end
def mode
  puts "mode"
  0
end
def opts(h)
  puts "opts"
  h
end
t("order, empty") { IO.sysopen(path(src), mode, 0644, **opts({})).is_a?(Integer) }
t("order, full") { IO.sysopen(path(src), mode, 0644, **opts({a: 1})) }
def fwd(p, **kw) = IO.sysopen(p, 0, 0644, **kw)
t("forwarded") { fwd(src).is_a?(Integer) }
t("forwarded, full") { fwd(src, a: 1) }
def fwd_anon(p, **) = IO.sysopen(p, 0, 0644, **)
t("anonymous") { fwd_anon(src).is_a?(Integer) }
t("anonymous, full") { fwd_anon(src, a: 1) }
r, w = IO.pipe
w.write "x"
t("select") { IO.select([r], nil, nil, 0, **e).size }
t("select, short") { IO.select([r], nil, nil, **e).size }
t("select, none") { IO.select(**e) }
t("File.select") { File.select([r], nil, nil, 0, **e).size }
t("copy_stream") { IO.copy_stream(src, File.join(Dir.tmpdir, "spinel_class_method_double_splat_dst.txt"), **{}) }
File.delete(src)
File.delete(File.join(Dir.tmpdir, "spinel_class_method_double_splat_dst.txt"))
