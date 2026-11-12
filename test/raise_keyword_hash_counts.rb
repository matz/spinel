# Kernel#raise, Kernel#fail and Fiber#raise take cause: and pass a keyword
# hash with any other key on as one more argument, so a fourth one is
# refused, whether the hash is written out or a `**` operand
def t(l)
  m = Fiber.new { Fiber.yield 1 }
  m.resume
  r = yield m
  puts "#{l}: #{r.inspect}"
rescue Exception => e
  puts "#{l}: #{e.class}: #{e.message}"
end
e = {}
f = {zz: 1}
g = {cause: nil}
h = {cause: nil, zz: 1}
t("fiber zz") { |m| m.raise(RuntimeError, "w", nil, zz: 1) }
t("fiber cause") { |m| m.raise(RuntimeError, "w", nil, cause: nil) }
t("fiber cause zz") { |m| m.raise(RuntimeError, "w", nil, cause: nil, zz: 1) }
t("fiber **e") { |m| m.raise(RuntimeError, "w", nil, **e) }
t("fiber **f") { |m| m.raise(RuntimeError, "w", nil, **f) }
t("fiber **g") { |m| m.raise(RuntimeError, "w", nil, **g) }
t("fiber **h") { |m| m.raise(RuntimeError, "w", nil, **h) }
t("kernel zz") { |m| raise(RuntimeError, "w", nil, zz: 1) }
t("kernel cause zz") { |m| raise(RuntimeError, "w", nil, cause: nil, zz: 1) }
t("kernel **e") { |m| raise(RuntimeError, "w", nil, **e) }
t("kernel **f") { |m| raise(RuntimeError, "w", nil, **f) }
t("kernel **g") { |m| raise(RuntimeError, "w", nil, **g) }
t("kernel **h") { |m| raise(RuntimeError, "w", nil, **h) }
t("fail zz") { |m| fail(RuntimeError, "w", nil, zz: 1) }
t("fail cause") { |m| fail(RuntimeError, "w", nil, cause: nil) }
t("fail **e") { |m| fail(RuntimeError, "w", nil, **e) }
t("fail **f") { |m| fail(RuntimeError, "w", nil, **f) }
t("fail **h") { |m| fail(RuntimeError, "w", nil, **h) }
t("Kernel.fail zz") { |m| Kernel.fail(RuntimeError, "w", nil, zz: 1) }
t("Kernel.fail **f") { |m| Kernel.fail(RuntimeError, "w", nil, **f) }
t("Kernel.raise **g") { |m| Kernel.raise(RuntimeError, "w", nil, **g) }
t("Kernel.raise **h") { |m| Kernel.raise(RuntimeError, "w", nil, **h) }
# past the most positionals the call is refused whatever the hash holds,
# and the message counts a hash with another key
t("kernel 4 **e") { |m| raise(RuntimeError, "w", nil, 1, **e) }
t("kernel 4 **f") { |m| raise(RuntimeError, "w", nil, 1, **f) }
t("kernel 4 **g") { |m| raise(RuntimeError, "w", nil, 1, **g) }
t("kernel 4 zz") { |m| raise(RuntimeError, "w", nil, 1, zz: 1) }
t("kernel 4 cause") { |m| raise(RuntimeError, "w", nil, 1, cause: nil) }
t("kernel 4 cause **e") { |m| raise(RuntimeError, "w", nil, 1, cause: nil, **e) }
t("fail 5 **f") { |m| fail(RuntimeError, "w", nil, 1, 2, **f) }
t("fiber 4 **e") { |m| m.raise(RuntimeError, "w", nil, 1, **e) }
t("fiber 4 **h") { |m| m.raise(RuntimeError, "w", nil, 1, **h) }
# pairs beside a `**` operand, and a literal `**` hash, count the same way
def v(x)
  print "<#{x.inspect}>"
  x
end
t("kernel 4 cause **f") { |m| raise(RuntimeError, "w", nil, 1, cause: nil, **f) }
t("kernel cause **e") { |m| raise(RuntimeError, "w", nil, cause: nil, **e) }
t("kernel cause **f") { |m| raise(RuntimeError, "w", nil, cause: nil, **f) }
t("kernel **g zz") { |m| raise(RuntimeError, "w", nil, **g, zz: 1) }
t("kernel **e cause") { |m| raise(RuntimeError, "w", nil, **e, cause: nil) }
t("kernel **{zz}") { |m| raise(RuntimeError, "w", nil, **{zz: 1}) }
t("kernel 4 **{zz}") { |m| raise(RuntimeError, "w", nil, 1, **{zz: 1}) }
t("kernel **{cause}") { |m| raise(RuntimeError, "w", nil, **{cause: nil}) }
t("fiber cause **f") { |m| m.raise(RuntimeError, "w", nil, cause: nil, **f) }
t("fiber cause **e") { |m| m.raise(RuntimeError, "w", nil, cause: nil, **e) }
t("fail 4 order") { |m| fail(RuntimeError, "w", nil, v(1), zz: v(2), **v(e)) }
t("kernel order") { |m| raise(RuntimeError, "w", nil, v(:a) => 1, **v(f)) }
x = [1].size > 5 ? 1 : (raise(RuntimeError, "w", nil, cause: nil, **e) rescue 9)
p x
# a hash of cause: only at two positionals is no third (a backtrace)
t("kernel 2 **g") { |m| raise(RuntimeError, "w", **g) }
# a key the run time computes is counted when it is known: `cause` is the
# keyword, any other key one more argument
k = :cause
z = :zz
t("kernel **{k}") { |m| raise(RuntimeError, "w", nil, **{k => nil}) }
t("kernel **{z}") { |m| raise(RuntimeError, "w", nil, **{z => 1}) }
t("fail **{k}") { |m| fail(RuntimeError, "w", nil, **{k => nil}) }
t("fiber **{k}") { |m| m.raise(RuntimeError, "w", nil, **{k => nil}) }
t("kernel **{k} **{}") { |m| raise(RuntimeError, "w", nil, **{k => nil}, **{}) }
t("kernel k pair") { |m| raise(RuntimeError, "w", nil, k => nil) }
t("kernel cause k pair") { |m| raise(RuntimeError, "w", nil, cause: nil, k => nil) }
t("kernel z pair") { |m| raise(RuntimeError, "w", nil, z => 1) }
t("kernel cause z pair") { |m| raise(RuntimeError, "w", nil, cause: nil, z => 1) }
t("fail z pair") { |m| fail(RuntimeError, "w", nil, z => 1) }
t("fiber z pair") { |m| m.raise(RuntimeError, "w", nil, z => 1) }
t("kernel string key") { |m| raise(RuntimeError, "w", nil, "cause" => nil) }
t("kernel **{string key}") { |m| raise(RuntimeError, "w", nil, **{"cause" => nil}) }
