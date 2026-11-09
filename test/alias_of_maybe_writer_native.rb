# The class a loader's rescue may or may not reach (alias_of_maybe_writer.rb):
# on its own it only defines the writer.
class Database
  def bt=(x); @v = [:native, x]; end
end
