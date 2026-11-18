# A conversion hook can create a Proc while evaluating an omitted default.
class C
 def to_str(x=->{"ok"});x.call;end
end
p("x"+C.new)
