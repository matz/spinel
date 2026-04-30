# Issue #127: a module-level singleton method (`module M; def self.X; ... end; end`)
# whose body returns a non-int value used to be typed as `mrb_int` at every
# call site. Capturing to a local emitted `mrb_int = const char *` (compile
# error under -Werror); inline `puts M.X` compiled but printed the pointer
# as an integer. The class form (same shape inside `class C`) already worked.
# Fix is in `infer_constant_recv_type`: walk `@meth_return_types` for
# `<Module>_cls_<method>` the same way the class branch walks `@cls_cmeth_returns`.

# 1. Module + def self + String return + local capture — the failing shape.
module M1
  def self.greet
    "hello"
  end
end

s = M1.greet
puts s                              # hello

# 2. Module + def self + String return + inline puts — same root, different consumer.
module M2
  def self.greet
    "world"
  end
end

puts M2.greet                       # world

# 3. Module + def self + Float return — non-int isn't string-specific.
module M3
  def self.pi
    3.14
  end
end

puts M3.pi                          # 3.14

# 4. Class + def self + String return — the path that already worked. Regression guard.
class C1
  def self.greet
    "from class"
  end
end

t = C1.greet
puts t                              # from class

# 5. Top-level def returning String — separate inference path. Regression guard.
def top_greet
  "top"
end

u = top_greet
puts u                              # top
