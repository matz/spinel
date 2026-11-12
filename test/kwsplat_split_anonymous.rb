# An anonymous `**` forwarded to a builtin that takes no keywords is split
# as a named one is: nothing for an empty hash, one more positional else
require "tmpdir"
def t(l)
  r = yield
  puts "#{l}: #{r.inspect}"
rescue => e
  puts "#{l}: #{e.class}: #{e.message}"
end
def fwda(x, **) = x.first(1, **)
def fwdc(x, **) = x.center(5, **)
def fwdh(**) = {**}
def fwde(**) = File.exist?("/", **)
def fwds(p, **) = IO.sysopen(p, 0, 0644, **)
a = [3, 1, 2]
t("fwda") { fwda(a) }
t("fwda f") { fwda(a, z: 1) }
t("fwdc") { fwdc("abc") }
t("fwdc f") { fwdc("abc", z: 1) }
t("fwdh") { fwdh(z: 1) }
t("fwde") { fwde }
t("fwde f") { fwde(z: 1) }
pa = File.join(Dir.tmpdir, "spinel_kwsplat_split_anonymous.txt")
File.write(pa, "abc")
t("fwds") { fwds(pa).is_a?(Integer) }
t("fwds f") { fwds(pa, z: 1) }
File.delete(pa)
