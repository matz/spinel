# A send of a computed name in method_missing compiles in a few inference
# rounds: the dispatch converts its name with to_sym (CRuby converts a
# send's name with to_str and does not send method_missing for that), and
# splitting that conversion on the class's own method_missing lowered the
# send again each round, until the 128-round cap (the delegate library's
# Delegator, reduced). infer-test checks the round count.
class Delegator < BasicObject
  def method_missing(m, *args, &block)
    r = true
    target = self.__getobj__ {r = false}
    if r && target_respond_to?(target, m, false)
      target.__send__(m, *args, &block)
    else
      super(m, *args, &block)
    end
  end
  def methods(all=true)
    __getobj__.methods(all) | super
  end
  @api = instance_methods(false)
end
p :ok
