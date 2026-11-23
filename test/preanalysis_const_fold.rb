# Pre-analysis constant-branch pruning preserves CRuby 4.0 semantics:
# dead branches guarded by provably fixed compile-time predicates are pruned
# before require resolution and type inference, while preserving lexical local
# variable declarations, nil return values, and conservative defined? behavior.

# 1. Lexical local-variable visibility in pruned branches (CRuby 4.0 parity)
if false
  x_false = 1
end
p [:false_local, x_false, defined?(x_false)]

if RUBY_PLATFORM.include?("wasi")
  require "spinel_nonexistent_wasi_gem"
  require_relative "preanalysis_const_fold/nonexistent"
  autoload :NonexistentWasiConst, "spinel_nonexistent_wasi_gem"
  x_dead = "server"
  y_dead ||= 99
end
p [:dead_local, x_dead, defined?(x_dead), binding.local_variable_get(:x_dead), y_dead, defined?(y_dead)]

if RUBY_PLATFORM.include?("wasi") && (z_short = 123)
  w_dead = 456
end
p [:short_circuit, z_short, defined?(z_short), w_dead, defined?(w_dead)]

def dead_locals_in_method
  if false
    m_false = 1
  end
  if RUBY_PLATFORM.include?("wasi")
    m_dead = "bad"
  end
  [m_false, defined?(m_false), m_dead, defined?(m_dead)]
end
p [:method_locals, dead_locals_in_method]

# 2. Exact nil return values for untaken if without else
v_nil = if RUBY_PLATFORM.include?("wasi")
  42
end
p [:expr_nil, v_nil]

def tail_nil(flag)
  return 1 if flag
  if RUBY_PLATFORM.include?("wasi")
    42
  end
end
p [:tail_nil, tail_nil(true), tail_nil(false)]

cls_val = class PreanalysisNilClass
  123
  if RUBY_PLATFORM.include?("wasi")
    456
  end
end
p [:cls_nil, cls_val]

# 3. Ternary, modifier if/unless, elsif, boolean ops, string predicates
p [:ternary, RUBY_PLATFORM.start_with?("wasm32") ? "w" : 42]
p [:end_with, RUBY_PLATFORM.end_with?("wasi") ? :wasi : :host]
p [:eq_ne, RUBY_PLATFORM == "wasm32-wasi", RUBY_PLATFORM != "wasm32-wasi"]
p [:elsif, if RUBY_PLATFORM.include?("wasi")
  :wasi
elsif !RUBY_PLATFORM.empty? && !RUBY_PLATFORM.nil?
  :host
else
  :empty
end]
p [:logic, !RUBY_PLATFORM.include?("wasi") && (RUBY_PLATFORM.include?("wasi") || defined?(String) == "constant")]
p [:const_defined, Object.const_defined?(:RUBY_PLATFORM) ? :yes : :no]

require "spinel_nonexistent_wasi_gem" if RUBY_PLATFORM.include?("wasi")
require "spinel_nonexistent_wasi_gem" unless !RUBY_PLATFORM.include?("wasi")

# 4. Dead branch does not widen live local or expression types
live_x = 10
if RUBY_PLATFORM.include?("wasi")
  live_x = "widen_me"
end
live_v = if !RUBY_PLATFORM.include?("wasi")
  live_x + 5
else
  "other"
end
p [:no_widen, live_v + 2]

# 5. Conservative defined?: never fold away constants defined later or via require
def check_later
  if defined?(PREANALYSIS_LATER_CONST)
    :found
  else
    :missing
  end
end
p [:later_before, check_later]
PREANALYSIS_LATER_CONST = 1
p [:later_after, check_later]

def check_required
  if defined?(PREANALYSIS_REQUIRED_CONST)
    PREANALYSIS_REQUIRED_CONST
  else
    :missing
  end
end
p [:req_before, check_required]
require_relative "preanalysis_const_fold/helper"
p [:req_after, check_required, PREANALYSIS_HELPER_DEAD_LOCAL]

# 6. Outer local in a lambda/block must NOT be shadowed by a dead write
outer_val = 77
fn = -> {
  if RUBY_PLATFORM.include?("wasi")
    outer_val = 99
    inner_only = 1
  end
  [outer_val, inner_only, defined?(inner_only)]
}
p [:lambda_dead_locals, fn.call]

# 7. BasicObject subclass: bare constants/const_defined? stay unfolded, :: works
class PreanalysisBasicSub < BasicObject
  def self.check
    bare = const_defined?(:String) ? :bare_def : :bare_undef
    obj_def = ::Object.const_defined?(:String) ? :obj_def : :obj_undef
    rooted = ::RUBY_PLATFORM.include?("wasi") ? :wasi : :host
    [bare, obj_def, rooted]
  end
end
p [:basic_object, PreanalysisBasicSub.check]

# 8. Pattern-matching guard safety (in pat if guard must not be pruned)
case 7
in n if !RUBY_PLATFORM.include?("wasi")
  p [:pat_guard, n]
end
