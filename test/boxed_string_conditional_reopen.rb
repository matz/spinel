module Layer
  def fresh_reader = 9
  private
  def size = 88
end
def sample(s)
  p [s.respond_to?(:fresh_reader), s.respond_to?(:size)]
  begin
    p s.fresh_reader
  rescue NoMethodError
    p :absent
  end
  begin
    p s.size
  rescue NoMethodError
    p :hidden
  end
end
s = +"abc"
sample(s)
if (ENV.fetch("SPINEL_TEST_EARLY_PREPEND", "yes") == "yes")
  class String
    prepend Layer
  end
end
sample(s)
