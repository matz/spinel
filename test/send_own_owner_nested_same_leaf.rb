# A boxed receiver split by the owner of its own send: the owner NS::K
# shares its last name with a top-level K, which must not take the call.
module NS
  class K
    def send(x) = "NS::K send #{x}"
  end
end
class K; def year = 7; end
def go(r) = r.send(:year)
[NS::K.new, K.new].each { |v| p go(v) }
