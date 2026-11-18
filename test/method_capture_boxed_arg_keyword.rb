# The boxed argument of method_capture_boxed_arg.rb, passed as a keyword's
# value: it reaches the parameter of that name. Each method is typed by a
# direct call with Integers first. As in a direct call, Gate#rest collects
# a keyword it does not declare in its `**kw`, and Gate#nokw, which
# declares no keyword, takes the keywords as a trailing positional Hash.
class Gate
  def opt(a, limit: 10) = [a, limit]
  def req(name:, level:) = [name, level]
  def rest(a, **kw) = [a, kw]
  def nokw(a, h) = [a, h]
end
def run(table, key, values)
  callback = table[key]
  case key
  when :opt_short then callback.call(values[0])
  when :req then callback.call(level: values[1], name: values[0])
  when :rest then callback.call(values[0], extra: values[1])
  when :nokw then callback.call(values[0], k: values[1])
  else callback.call(values[0], limit: values[1])
  end
end
gate = Gate.new
p gate.opt(1, limit: 2), gate.req(name: 1, level: 2), gate.rest(1, extra: 2), gate.nokw(1, {k: 2})
table = {opt: gate.method(:opt), opt_short: gate.method(:opt), req: gate.method(:req),
         rest: gate.method(:rest), nokw: gate.method(:nokw), lam: ->(a, limit: 0) { [a, limit] }}
values = [1.5, "s", nil]
p run(table, :opt, values), run(table, :opt_short, values), run(table, :req, values)
p run(table, :rest, values), run(table, :nokw, values), run(table, :lam, values)
p gate.opt(3, limit: 4), gate.req(name: "n", level: 5)

module Policy
  def self.check(value:)
    p value.is_a?(Float)
    value
  end
end
def adapter(options)
  callback = options[:loader]
  proc { |value| callback.call(value: value) }
end
Policy.check(value: 0)
method_loader = adapter({loader: Policy.method(:check)})
proc_loader = adapter({loader: ->(value:) { value }})
p method_loader.call(1.5)
p proc_loader.call("s")
