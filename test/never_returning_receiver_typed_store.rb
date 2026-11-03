# A call on a receiver that never returns (a method that only raises) is
# evaluated for its raise. Stored into a slot typed by its other writes, it
# answers that slot's placeholder, not an untyped one's boxed nil.

class Admin;  def url_options = raise(NotImplementedError, "admin"); end
class Public; def url_options = raise(NotImplementedError, "public"); end

def unavailable = raise(ArgumentError, "unavailable")

class Helper
  def initialize
    @options = { only_path: true }
    @names = ["a"]
    @label = "x"
  end

  # a local typed as a Symbol-keyed hash by its later store
  def call(t)
    options = t.url_options.merge @options
    options[:host] = "example.com"
    options
  end

  # ivars typed by their initialize
  def rename = @names = unavailable.map(&:to_s)
  def relabel = @label = unavailable.upcase

  attr_reader :names, :label
end

[Admin.new, Public.new].each do |t|
  p Helper.new.call(t)
rescue NotImplementedError => e
  p e.message
end

h = Helper.new
[-> { h.rename }, -> { h.relabel }].each do |f|
  f.call
rescue ArgumentError => e
  p e.message
end
p h.names, h.label
