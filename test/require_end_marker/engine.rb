if RUBY_ENGINE == "jruby"
  require_relative "no_such_file"
end
puts "engine.rb loaded"
__END__
RUBY_ENGINE = 1
