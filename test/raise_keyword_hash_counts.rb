# Kernel#raise and Fiber#raise take cause: and pass a keyword hash with any
# other key on as one more argument, so a fourth one is refused, whether the
# hash is written out or a `**` operand
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
# a hash of cause: only at two positionals is no third (a backtrace)
t("kernel 2 **g") { |m| raise(RuntimeError, "w", **g) }
