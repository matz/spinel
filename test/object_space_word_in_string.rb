# The word define_finalizer in a string literal or a comment is not a call
# of it: the finalizer API is spliced for the identifier only, so a program
# that merely prints the word is compiled as if it had not.
puts "this program does not call define_finalizer"
def read_env(line)
  return nil if line.empty?
  { "PATH_INFO" => line }
end

class App
  def call(env) = %w[PATH_INFO X].map { |k| "#{k}=#{env[k]}" }
end

app = App.new
["/a", ""].each do |line|
  env = read_env(line)
  p env ? app.call(env) : nil
end

out = []
stream = ->(s) { s << "!" }
stream.call(out)
p out
