# frozen_string_literal: true

class Date
  # Bitmask flag constants for strftime parsing
  FL_LEFT   = 0x01  # '-' flag
  FL_SPACE  = 0x02  # '_' flag
  FL_ZERO   = 0x04  # '0' flag
  FL_UPPER  = 0x08  # '^' flag
  FL_CHCASE = 0x10  # '#' flag
  private_constant :FL_LEFT, :FL_SPACE, :FL_ZERO, :FL_UPPER, :FL_CHCASE

  # call-seq:
  #   strftime(format = '%F') -> string
  #
  # Returns a string representation of the date in +self+,
  # formatted according the given +format+:
  #
  #   Date.new(2001, 2, 3).strftime # => "2001-02-03"
  #
  # For other formats, see
  # {Formats for Dates and Times}[rdoc-ref:language/strftime_formatting.rdoc].
  def strftime(format = DEFAULT_STRFTIME_FMT)
    if format.equal?(DEFAULT_STRFTIME_FMT)
      internal_civil unless @year
      return fast_ymd.force_encoding(Encoding::US_ASCII)
    end
    fmt = Date.__send__(:string_value, format)
    if fmt == YMD_FMT
      internal_civil unless @year
      return fast_ymd.force_encoding(Encoding::US_ASCII)
    end
    internal_strftime(fmt).force_encoding(fmt.encoding)
  end

  private

  def internal_strftime(fmt)
    # Fast path for common format strings (whole-string match)
    case fmt
    when '%Y-%m-%d', '%F'
      internal_civil unless @year
      return fast_ymd
    when '%H:%M:%S', '%T', '%X'
      return instance_of?(Date) ? +'00:00:00' : "#{PAD2[internal_hour]}:#{PAD2[internal_min]}:#{PAD2[internal_sec]}"
    when '%m/%d/%y', '%D', '%x'
      internal_civil unless @year
      return "#{PAD2[@month]}/#{PAD2[@day]}/#{PAD2[@year % 100]}"
    when '%Y-%m-%dT%H:%M:%S%z'
      internal_civil unless @year
      if instance_of?(Date)
        return fast_ymd << 'T00:00:00+0000'
      else
        of = of_seconds
        sign = of < 0 ? '-' : '+'
        abs = of.abs
        return "#{fast_ymd}T#{PAD2[internal_hour]}:#{PAD2[internal_min]}:#{PAD2[internal_sec]}#{sign}#{PAD2[abs / 3600]}#{PAD2[(abs % 3600) / 60]}"
      end
    when '%a %b %e %H:%M:%S %Y', '%c'
      internal_civil unless @year
      return fmt_asctime_str
    when '%A, %B %d, %Y'
      internal_civil unless @year
      w = (@jd + 1) % 7
      y = @year
      y_s = y >= 1000 ? y.to_s : (y >= 0 ? format('%04d', y) : format('-%04d', -y))
      return "#{DAY_FULL_COMMA[w]}#{MONTH_FULL_SPACE[@month]}#{PAD2[@day]}, #{y_s}"
    end

    result = +''.encode(fmt.encoding)
    sc = StringScanner.new(fmt.dup)

    until sc.eos?
      # Batch collect literal (non-%) characters
      if (lit = sc.scan(/[^%]+/))
        result << lit
        next
      end

      # Must be '%' or something unexpected
      unless sc.skip(/%/)
        result << sc.getch
        next
      end

      if sc.eos?
        result << '%'
        break
      end

      # Quick dispatch: if next char is a simple letter spec (A-Z excluding E/O,
      # or a-z), skip flag/width/prec parsing entirely.
      if (quick = sc.scan(/[A-DFG-NP-Za-z]/))
        fast = fast_spec(quick.ord)
        if fast
          result << fast
          next
        end
        sc.unscan
      end

      # Parse flags (bitmask)
      flags = 0
      colons = 0
      while (f = sc.scan(/[-_0^#]/))
        case f
        when '-' then flags |= FL_LEFT
        when '_' then flags |= FL_SPACE
        when '0' then flags |= FL_ZERO
        when '^' then flags |= FL_UPPER
        when '#' then flags |= FL_CHCASE
        end
      end
      if (c = sc.scan(/:+/))
        colons = c.length
      end

      # Parse width
      width = sc.scan(/\d+/)&.to_i
      raise Errno::ERANGE, "strftime" if width && width > STRFTIME_MAX_WIDTH

      # Parse precision (after '.')
      prec = sc.skip(/\./) ? (sc.scan(/\d+/)&.to_i || 0) : nil

      # Post-width colons (for %8:z, %11::z etc.)
      if (c = sc.scan(/:+/))
        colons += c.length
      end

      # Locale modifier (%E or %O)
      locale_mod = sc.scan(/[EO]/)&.ord

      spec = sc.getch&.ord

      # Inline fast path: no flags, no width, no prec, no locale mod, no colons
      if flags == 0 && width.nil? && prec.nil? && locale_mod.nil? && colons == 0
        fast = fast_spec(spec)
        if fast
          result << fast
          next
        end
      end

      result << format_spec_b(spec, flags, colons, width, prec, locale_mod)
    end

    result
  end

  # Format "%a %b %e %H:%M:%S %Y" directly (used by %c expansion and asctime)
  def fmt_asctime_str
    w = (@jd + 1) % 7
    d = @day
    d_s = d < 10 ? " #{d}" : d.to_s
    y = @year
    y_s = y >= 1000 ? y.to_s : (y >= 0 ? format('%04d', y) : format('-%04d', -y))
    if instance_of?(Date)
      "#{ASCTIME_PREFIX[w][@month]}#{d_s} 00:00:00 #{y_s}"
    else
      "#{ASCTIME_PREFIX[w][@month]}#{d_s} #{PAD2[internal_hour]}:#{PAD2[internal_min]}:#{PAD2[internal_sec]} #{y_s}"
    end
  end

  # Inline fast path for common specs with default formatting (no flags/width/prec)
  def fast_spec(spec)
    case spec
    when 89  # 'Y'
      internal_civil unless @year
      y = @year
      if y >= 1000
        s = y.to_s
        raise Errno::ERANGE, "strftime" if s.length >= STRFTIME_MAX_COPY_LEN
        s
      elsif y >= 0
        format('%04d', y)
      else
        format('-%04d', -y)
      end
    when 109 # 'm'
      internal_civil unless @year
      PAD2[@month]
    when 100 # 'd'
      internal_civil unless @year
      PAD2[@day]
    when 101 # 'e'
      internal_civil unless @year
      d = @day
      d < 10 ? " #{d}" : d.to_s
    when 72  # 'H'
      PAD2[internal_hour]
    when 77  # 'M'
      PAD2[internal_min]
    when 83  # 'S'
      PAD2[internal_sec]
    when 97  # 'a'
      STRFTIME_DAYS_ABBR[(@jd + 1) % 7]
    when 65  # 'A'
      STRFTIME_DAYS_FULL[(@jd + 1) % 7]
    when 98, 104  # 'b', 'h'
      internal_civil unless @year
      STRFTIME_MONTHS_ABBR[@month]
    when 66  # 'B'
      internal_civil unless @year
      STRFTIME_MONTHS_FULL[@month]
    when 112 # 'p'
      internal_hour < 12 ? 'AM' : 'PM'
    when 80  # 'P'
      internal_hour < 12 ? 'am' : 'pm'
    when 37  # '%'
      '%'
    when 110 # 'n'
      "\n"
    when 116 # 't'
      "\t"
    when 106 # 'j'
      yd = yday
      if yd < 10
        "00#{yd}"
      elsif yd < 100
        "0#{yd}"
      else
        yd.to_s
      end
    when 119 # 'w'
      ((@jd + 1) % 7).to_s
    when 117 # 'u'
      cwday.to_s
    when 121 # 'y'
      internal_civil unless @year
      PAD2[@year % 100]
    when 90  # 'Z'
      zone_str
    when 122 # 'z'
      of = of_seconds
      sign = of < 0 ? '-' : '+'
      abs = of.abs
      "#{sign}#{PAD2[abs / 3600]}#{PAD2[(abs % 3600) / 60]}"
    when 115 # 's'
      ((@jd - 2440588) * 86400 - of_seconds + internal_hour * 3600 + internal_min * 60 + internal_sec).to_s
    # Composite specs — inline expansion
    when 99  # 'c'
      internal_civil unless @year
      fmt_asctime_str
    when 70  # 'F'
      internal_civil unless @year
      fast_ymd
    when 84, 88  # 'T', 'X'
      instance_of?(Date) ? '00:00:00' : "#{PAD2[internal_hour]}:#{PAD2[internal_min]}:#{PAD2[internal_sec]}"
    when 68, 120  # 'D', 'x'
      internal_civil unless @year
      "#{PAD2[@month]}/#{PAD2[@day]}/#{PAD2[@year % 100]}"
    when 82  # 'R'
      instance_of?(Date) ? '00:00' : "#{PAD2[internal_hour]}:#{PAD2[internal_min]}"
    when 114  # 'r'
      if instance_of?(Date)
        '12:00:00 AM'
      else
        h = internal_hour % 12
        h = 12 if h == 0
        "#{PAD2[h]}:#{PAD2[internal_min]}:#{PAD2[internal_sec]} #{internal_hour < 12 ? 'AM' : 'PM'}"
      end
    else
      nil  # fall through to format_spec_b
    end
  end

  # Full spec handling with bitmask flags (called when fast_spec returns nil or flags/width/prec present)
  def format_spec_b(spec, flags, colons, width, prec, locale_mod)
    # Handle %E/%O locale modifiers
    if locale_mod
      valid = locale_mod == 69 ? STRFTIME_E_VALID_BYTES : STRFTIME_O_VALID_BYTES  # 69='E'
      unless valid.include?(spec)
        mod_chr = locale_mod == 69 ? 'E' : 'O'
        return "%#{mod_chr}#{spec&.chr}"
      end
    end

    case spec
    when 89, 71  # 'Y', 'G'
      y = spec == 89 ? year : cwyear
      fmt_year(y, width, prec, flags)
    when 67  # 'C'
      cent = year.div(100)
      pad_num(cent, width || 2, flags)
    when 121 # 'y'
      pad_num(year % 100, width || 2, flags, zero: true)
    when 103 # 'g'
      pad_num(cwyear % 100, width || 2, flags, zero: true)
    when 109 # 'm'
      pad_num(month, width || 2, flags, zero: true)
    when 100 # 'd'
      pad_num(day, width || 2, flags, zero: true)
    when 101 # 'e'
      pad_num(day, width || 2, flags, zero: false)
    when 106 # 'j'
      pad_num(yday, width || 3, flags, zero: true)
    when 72  # 'H'
      pad_num(internal_hour, width || 2, flags, zero: true)
    when 107 # 'k'
      pad_num(internal_hour, width || 2, flags, zero: false)
    when 73  # 'I'
      h = internal_hour % 12
      h = 12 if h == 0
      pad_num(h, width || 2, flags, zero: true)
    when 108 # 'l'
      h = internal_hour % 12
      h = 12 if h == 0
      pad_num(h, width || 2, flags, zero: false)
    when 77  # 'M'
      pad_num(internal_min, width || 2, flags, zero: true)
    when 83  # 'S'
      pad_num(internal_sec, width || 2, flags, zero: true)
    when 76  # 'L'
      w = width || 3
      ms = (sec_frac * (10**w)).floor
      ms.to_s.rjust(w, '0')
    when 78  # 'N'
      w = width || prec || 9
      ns = (sec_frac * (10**w)).floor
      ns.to_s.rjust(w, '0')
    when 115 # 's'
      unix = (@jd - 2440588) * 86400 - of_seconds +
             internal_hour * 3600 + internal_min * 60 + internal_sec
      pad_num(unix, width || 1, flags)
    when 81  # 'Q'
      ms = ((@jd - 2440588) * 86400 - of_seconds +
            internal_hour * 3600 + internal_min * 60 + internal_sec) * 1000 +
           (sec_frac * 1000).floor
      pad_num(ms, width || 1, flags)
    when 65  # 'A'
      fmt_str(STRFTIME_DAYS_FULL[wday], width, flags)
    when 97  # 'a'
      fmt_str(STRFTIME_DAYS_ABBR[wday], width, flags)
    when 66  # 'B'
      fmt_str(STRFTIME_MONTHS_FULL[month], width, flags)
    when 98, 104  # 'b', 'h'
      fmt_str(STRFTIME_MONTHS_ABBR[month], width, flags)
    when 112 # 'p'
      fmt_str(internal_hour < 12 ? 'AM' : 'PM', width, flags)
    when 80  # 'P'
      fmt_str(internal_hour < 12 ? 'am' : 'pm', width, flags)
    when 90  # 'Z'
      fmt_str(zone_str, width, flags)
    when 122 # 'z'
      fmt_z(colons, width, prec, flags)
    when 117 # 'u'
      pad_num(cwday, width || 1, flags)
    when 119 # 'w'
      pad_num(wday, width || 1, flags)
    when 85  # 'U'
      pad_num(week_number(0), width || 2, flags, zero: true)
    when 87  # 'W'
      pad_num(week_number(1), width || 2, flags, zero: true)
    when 86  # 'V'
      pad_num(cweek, width || 2, flags, zero: true)
    when 37  # '%'
      '%'
    when 43  # '+'
      s = internal_strftime('%a %b %e %H:%M:%S %Z %Y')
      fmt_str(s, width, flags)
    else
      # Try composite
      expansion = STRFTIME_COMPOSITE_BYTE[spec]
      if expansion
        s = internal_strftime(expansion)
        fmt_str(s, width, flags)
      else
        "%#{spec&.chr}"
      end
    end
  end

  # Format year (handles negative years, precision, and all flag variants)
  def fmt_year(y, width, prec, flags)
    if prec
      s = y.abs.to_s.rjust(prec, '0')
      s = (y < 0 ? '-' : '') + s
    elsif flags & FL_LEFT != 0
      s = (y < 0 ? '-' : '') + y.abs.to_s
    else
      default_w = y < 0 ? 5 : 4
      w = width || default_w
      if flags & FL_SPACE != 0
        raw = (y < 0 ? '-' : '') + y.abs.to_s
        s = raw.rjust(w, ' ')
      else
        s = y.abs.to_s.rjust(w - (y < 0 ? 1 : 0), '0')
        s = (y < 0 ? '-' : '') + s
      end
    end
    raise Errno::ERANGE, "strftime" if s.length >= STRFTIME_MAX_COPY_LEN
    if flags & (FL_UPPER | FL_CHCASE) != 0
      s.upcase
    else
      s
    end
  end

  def pad_num(n, default_w, flags, zero: nil)
    sign = n < 0 ? '-' : ''
    abs  = n.abs.to_s
    w    = default_w

    if flags & FL_LEFT != 0
      sign + abs
    elsif flags & FL_SPACE != 0 || (flags & FL_ZERO == 0 && zero == false)
      sign + abs.rjust(w - sign.length, ' ')
    else
      pad = (flags & FL_ZERO != 0 || zero) ? '0' : ' '
      sign + abs.rjust([w - sign.length, abs.length].max, pad)
    end
  end

  def fmt_str(s, width, flags)
    s = s.dup
    if flags & FL_CHCASE != 0
      s = (s == s.upcase) ? s.downcase : s.upcase
    elsif flags & FL_UPPER != 0
      s = s.upcase
    end
    if flags & FL_LEFT != 0
      s
    elsif width
      pad = flags & FL_ZERO != 0 ? '0' : ' '
      s.rjust(width, pad)
    else
      s
    end
  end

  # Week number (0=first partial week)
  def week_number(ws)
    yd   = yday
    wd   = wday  # 0=Sun
    if ws == 1
      # Monday-based
      wd = wd == 0 ? 6 : wd - 1
    end
    (yd - wd + 6).div(7)
  end

  # Format %z with colons variant and GNU extension flag support
  def fmt_z(colons, width, _prec, flags)
    of   = of_seconds
    sign = of < 0 ? '-' : '+'
    abs  = of.abs
    hh   = abs / 3600
    mm   = (abs % 3600) / 60
    ss   = abs % 60

    no_lead = flags & (FL_LEFT | FL_SPACE) != 0

    if no_lead
      case colons
      when 0
        s = format('%s%d%02d', sign, hh, mm)
      when 1
        s = format('%s%d:%02d', sign, hh, mm)
      when 2
        s = format('%s%d:%02d:%02d', sign, hh, mm, ss)
      when 3
        if ss != 0
          s = format('%s%d:%02d:%02d', sign, hh, mm, ss)
        elsif mm != 0
          s = format('%s%d:%02d', sign, hh, mm)
        else
          s = format('%s%d', sign, hh)
        end
      else
        s = format('%s%d:%02d', sign, hh, mm)
      end
    else
      case colons
      when 0
        s = format('%s%02d%02d', sign, hh, mm)
      when 1
        s = format('%s%02d:%02d', sign, hh, mm)
      when 2
        s = format('%s%02d:%02d:%02d', sign, hh, mm, ss)
      when 3
        if ss != 0
          s = format('%s%02d:%02d:%02d', sign, hh, mm, ss)
        elsif mm != 0
          s = format('%s%02d:%02d', sign, hh, mm)
        else
          s = format('%s%02d', sign, hh)
        end
      else
        s = format('%s%02d:%02d', sign, hh, mm)
      end
    end

    if width
      if flags & FL_LEFT != 0
        s
      elsif flags & FL_SPACE != 0
        s.rjust(width, ' ')
      else
        digits = s[1..]
        sign + digits.rjust(width - 1, '0')
      end
    else
      s
    end
  end

  # Fast path helper: format '%Y-%m-%d' without allocation overhead
  def fast_ymd
    y = @year
    suffix = MONTH_DAY_SUFFIX[@month][@day]
    if y >= 1000
      y.to_s << suffix
    elsif y >= 0
      format('%04d', y) << suffix
    else
      format('-%04d', -y) << suffix
    end
  end

  # Helpers for DateTime override
  def internal_hour
    0
  end

  def internal_min
    0
  end

  def internal_sec
    0
  end

  def sec_frac
    0r
  end

  def of_seconds
    0
  end

  def zone_str
    '+00:00'
  end

end
