class B; def s; @x = 1; end; end
class C < B; end
begin
  C.new.freeze.s
rescue FrozenError => e
  puts e.message.sub(/0x\h+/, "X")
end
