# `raise "msg"` is `raise RuntimeError, "msg"`: a RuntimeError reopening's
# own initialize runs on the exception it raises, in every spelling of the
# form (fail, Kernel.raise, an interpolated or held String, with cause:,
# inside a rescue). Raised by name, it skipped the initialize.
$log = []
class RuntimeError
  def initialize(m = nil)
    super(m)
    $log << m
  end
end
def try
  yield
rescue => e
  [e.class, e.message, e.cause&.message]
end
p try { raise "plain" }
p try { fail "failed" }
n = 3
p try { raise "n=#{n}" }
s = "from var"
p try { raise s }
p try { Kernel.raise "kernel" }
p try { raise "with cause", cause: ArgumentError.new("why") }
p try { begin; raise ArgumentError, "inner"; rescue; raise "outer"; end }
p $log
