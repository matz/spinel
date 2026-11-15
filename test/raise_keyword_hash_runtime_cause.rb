# raise / fail with a keyword hash whose keys only the run time knows
# (`k => v` with k a variable, `**h`) or with cause: beside other keys:
# :cause comes out of the hash, and what is left, when anything is, is one
# more positional argument.
def t(label)
  yield
rescue => e
  puts "#{label}: #{e.class}: #{e.message.inspect} cause=#{e.cause.inspect}"
end
k = :cause
x = :x
c0 = StandardError.new("c0")
h = {cause: nil}
h2 = {cause: c0}
h3 = {}
h4 = {x: 1}
t("obj k=>nil") { raise(RuntimeError.new("e"), k => nil) }
t("obj x=>nil") { raise(RuntimeError.new("e"), x => nil) }
t("cls msg k=>c0") { raise(RuntimeError, "m", k => c0) }
t("cls msg x=>1") { raise(RuntimeError, "m", x => 1) }
t("str k=>c0") { raise("m", k => c0) }
t("obj **h") { raise(RuntimeError.new("e"), **h) }
t("cls msg **h2") { raise(RuntimeError, "m", **h2) }
t("cls msg **h3") { raise(RuntimeError, "m", **h3) }
t("cls msg **h4") { raise(RuntimeError, "m", **h4) }
t("cls **h2") { raise(RuntimeError, **h2) }
t("fail k=>c0") { fail(RuntimeError, "m", k => c0) }
t("lit cause and x") { raise(RuntimeError, "m", cause: c0, x: 1) }
t("string key") { raise(RuntimeError.new("e"), "cause" => 1) }
t("interp key") { raise(RuntimeError.new("e"), "#{k}": c0) }
# inside a rescue: no :cause keeps $! as the cause, cause: nil drops it
begin
  raise "outer"
rescue
  t("in rescue **h3") { raise(RuntimeError, "m", **h3) }
  t("in rescue **h") { raise(RuntimeError, "m", **h) }
  t("in rescue x=>1") { raise(RuntimeError.new("e"), x => 1) }
end
# order: the class, the message, then the hash
log = []
t("order") { raise((log << :k; RuntimeError), (log << :m; "m"), (log << :h; k) => c0) }
p log
# a class with its own exception
class MyErr < StandardError
  def self.exception(m = nil) = new("own #{m}")
end
t("own exception") { raise(MyErr, "m", k => c0) }
def kwfwd(**o) = raise(ArgumentError, "a", **o)
t("forwarded") { kwfwd(cause: c0) }
t("forwarded empty") { kwfwd }
t("obj cause and x") { raise(RuntimeError.new("e"), k => c0, x => 1) }
t("str cause and x") { raise("m", k => c0, x => 1) }
sm = "s"
t("str var cause and x") { raise(sm, x: 1, cause: c0) }
t("str var k and x") { raise(sm, :x => 1, k => c0) }
def gm = "g"
t("str call cause and x") { raise(gm, x: 1, cause: c0) }
ev = RuntimeError.new("v")
t("obj var cause and x") { raise(ev, x: 1, cause: c0) }
