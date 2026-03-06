local EVENT_NOTICE = "[event] spring chat event is live"

function on_login(ctx)
  if not ctx or not ctx.session_id then
    return { decision = "pass" }
  end

  local name = server.get_user_name(ctx.session_id)
  local online_count = server.get_online_count()

  if name and name ~= "" then
    server.send_notice(ctx.session_id, "welcome back, " .. name)
  else
    server.send_notice(ctx.session_id, "welcome to Knights")
  end

  server.send_notice(
    ctx.session_id,
    EVENT_NOTICE .. " | online=" .. tostring(online_count)
  )

  return { decision = "pass" }
end
