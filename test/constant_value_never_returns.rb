# A constant whose value never returns (a raise, or a call to a method that
# only raises) is never set: its write raises, `defined?` answers nil, and a
# read raises NameError. Its slot and its reads still build.

module Shim
  def self.lambda_of(&block) = raise(NotImplementedError, "no shareable lambda")
end

def unavailable = raise(ArgumentError, "unavailable")

class Router
  begin
    PATH = Shim.lambda_of { |o| o.to_s }
  rescue NotImplementedError => e
    puts "PATH: #{e.message}"
  end

  def helper = PATH
end

begin
  LIMIT = unavailable
rescue ArgumentError => e
  puts "LIMIT: #{e.message}"
end

begin
  DIRECT = raise(KeyError, "direct")
rescue KeyError => e
  puts "DIRECT: #{e.message}"
end

def limit = LIMIT

p defined?(Router::PATH), defined?(LIMIT), defined?(DIRECT)

[-> { Router.new.helper }, -> { limit }, -> { DIRECT }].each do |read|
  read.call
rescue NameError => e
  puts "#{e.class}: #{e.message}"
end

LATER = 7
p LATER
