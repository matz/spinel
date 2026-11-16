# A library built into a shared object over the position-independent
# runtime (lib/libspinel_rt_pic.a), which a host loads with dlopen: the
# regular archive is built for an executable, and `cc -shared` refused its
# relocations. test/ext/pic_host.c calls each entry through dlsym.
module ExtPic
  #: (Integer) -> Integer
  def self.triple(x) = x * 3

  # allocates in the shared object's runtime, and its collector runs there
  #: (Integer) -> String
  def self.label(n)
    parts = []
    n.times { |i| parts << "p#{i}" }
    parts.join(",")
  end

  #: (Integer) -> Integer
  def self.must_pos(x)
    raise ArgumentError, "not positive: #{x}" if x <= 0
    x
  end
end
