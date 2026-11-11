# `@x ||= v` and `@x &&= v` on an ivar with a slot store into the object, so a
# frozen receiver refuses them where they would write, as `@x = v` does.
class G
  def memo
    @z ||= 5
    :done
  end

  def value_memo
    @w ||= 6
  end

  def keep
    @k &&= 7
    :kept
  end

  def set_k
    @k = 1
  end
end
g = G.new
p g.memo
p g.value_memo
g.set_k
g.freeze
p g.memo
p g.value_memo
[:memo, :value_memo, :keep].each do |m|
  begin
    G.new.tap { |x| x.set_k }.freeze.public_send(m)
    p [m, :written]
  rescue FrozenError => e
    p [m, e.class]
  end
end
