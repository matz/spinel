# The order two includes of one class take is the order the load phase runs
# them, not the order of the nodes in the table: B is included by a required
# file before A is included here, so A is nearer, and A's `def zork(...) = super`
# reaches B#zork (a keyword and an optional), not Base's. Under
# --load-model=graph the required file's statements come after this file's in
# the table.

class Base
  def zork(a) = [:base, a]
end
module A
  def zork(...) = super
end
class C < Base; end
require_relative "module_super_forward_required_include/b"
class C
  include A
end
p C.new.zork(1, k: 5)
p C.new.zork(1, 9)
