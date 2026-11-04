# The builtin-only guard must not bypass a user reflection override,
# including a non-Boolean answer for a name the builtin values lack.
class ProtocolAnswer
  def respond_to?(name, include_all = false)
    name == :to_ary ? :custom_answer : nil
  end
end

def probe(value)
  p value.respond_to?(:to_ary)
  p value.respond_to?(:absent_protocol)
end
[7, 1.5, :sym, nil, true, false, "s", [], ProtocolAnswer.new].each { |value| probe(value) }
