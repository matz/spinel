# defined? on a Process::RLIMIT_* constant the program defines itself
module Process
  RLIMIT_SIGPENDING = 123
end
p Process::RLIMIT_SIGPENDING
p defined?(Process::RLIMIT_SIGPENDING)
