# spinel: rbs-seed-run
# An --rbs-pinned Array ivar keeps its pin's kind when the Array it is
# given comes from an allocation site another call's result also holds with
# other element kinds (#12009): the store converts into the pin, so the
# site is copied there, and the local that reads the ivar holds that copy.
# Since the names of one site took one container kind, the site joined the
# local's group, the local went boxed, and its read of the typed ivar was
# refused ("unsupported widening a typed array READ from an object").
class PinnedIvarRel
  def list_of(s)
    out = []
    out << s
    out
  end

  def order!(term)
    @orders = list_of(term)
  end

  def size
    prior = @orders
    prior.length
  end

  def first = @orders.first
end

r = PinnedIvarRel.new
r.order!("id")
mixed = r.list_of("x")
mixed << :sym
p r.size
p r.first
p mixed
