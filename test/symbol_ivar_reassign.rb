# Reassigning an instance variable to a different symbol used to demote
# its type to `poly` because `infer_ivar_init_type` reported `:foo` as
# `string` while `infer_type` reported it as `symbol`. The mismatch
# tripped `update_ivar_type`'s "old != new_type" branch, widening the
# field to `sp_RbVal`. Subsequent assignments emitted `self->s = SPS_bar`
# (sp_sym = mrb_int) into an `sp_RbVal` slot — a C type error.

class C
  def initialize
    @s = :foo
  end

  def reset
    @s = :bar
  end

  def show
    @s
  end
end

c = C.new
puts c.show
c.reset
puts c.show
