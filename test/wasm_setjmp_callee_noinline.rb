# On wasm32 a method that a method with a rescue calls is kept out of it
# (SP_NOINLINE) when, inlined, it would bring two or more calls of its own
# into the function that holds the setjmp; a short one is left to the C
# compiler. What the calls do must not change: a raise in a callee reaches
# the caller's rescue, a retry runs the callees again, the ensure runs, and
# a local the caller writes before the raise is read after it.
# spinel: wasm
# spinel: wasm-c-noinline: sp_Handler_prepare sp_Handler_route
# spinel: wasm-c-inline: sp_Handler_ok_p
class Handler
  attr_reader :log

  def initialize
    @log = []
    @tries = 0
  end

  def prepare(req)
    @log << "prepare #{req}"
    raise ArgumentError, "bad #{req}" if req.start_with?("x")
    req.upcase
  end

  def route(s)
    @tries += 1
    @log << "route #{s} #{@tries}"
    raise IOError, "again" if @tries == 1 && s == "RETRY"
    s.reverse
  end

  def ok?(n) = n >= 0 && n < 10

  def call(req)
    step = :start
    s = prepare(req)
    step = :prepared
    r = route(s)
    [ok?(r.size), r]
  rescue ArgumentError => e
    [:bad, step, e.message]
  rescue IOError
    retry
  ensure
    @log << "ensure #{req}"
  end
end

h = Handler.new
p h.call("abc")
p h.call("xyz")
h2 = Handler.new
p h2.call("retry")
p h2.call("a-much-longer-request")
puts h.log
puts h2.log
