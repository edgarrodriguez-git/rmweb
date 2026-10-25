"""HTML for the fake portal's variants (spec section 9). Pure functions: no server state in here.

Plain, high-contrast, large-type pages that read well on an e-ink panel. Every form action and fetch URL is
relative to /portal/<variant>/, so the same page works on any of the portal's origins.
"""
from __future__ import annotations

from html import escape

VARIANTS = (
    "plain-form", "room-name", "js-checkbox", "fetch-dom", "delayed-grant", "meta-refresh",
    "cross-origin", "relative-location", "status-511", "status-403",
    "target-blank-post", "window-open", "anchor-hash", "div-role-button", "terms-link",
    "tls-portal", "tls-two-hosts", "get-token", "long-select", "non-ascii",
)

_CSS = (
    "body{font:28px/1.45 sans-serif;color:#000;background:#fff;margin:24px}"
    "h1{font-size:40px}input,select,button{font:inherit;padding:10px 14px;margin:8px 0;border:3px solid #000;"
    "background:#fff;color:#000}input[type=checkbox]{width:32px;height:32px;vertical-align:middle}"
    "button:disabled{border-style:dashed;color:#666}label{display:block;margin:10px 0}"
    "a{color:#000}.btn{display:inline-block;border:3px solid #000;padding:12px 18px;margin:8px 0}"
    ".note{border-left:6px solid #000;padding-left:12px}"
)


def document(title: str, body: str, head: str = "") -> str:
    return (
        '<!doctype html><html lang="en"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        f"<title>{escape(title)}</title><style>{_CSS}</style>{head}</head><body>{body}</body></html>"
    )


def _heading(variant: str) -> str:
    return f"<h1>Fake Hotel Wi-Fi</h1><p><small>fixture: {escape(variant)}</small></p>"


def _notice(text: str) -> str:
    return f'<p class="note">{escape(text)}</p>' if text else ""


def _form(fields, *, action="accept", method="post", button="Connect", extra="", attrs=""):
    inputs = "".join(
        f'<label>{escape(label)}<br><input type="text" name="{name}" value="{escape(value)}"></label>'
        for name, label, value in fields
    )
    return (f'<form method="{method}" action="{action}"{attrs}>{inputs}{extra}'
            f'<button type="submit">{escape(button)}</button></form>')


def plain_form(variant, *, action="accept", notice="") -> str:
    fields = [("name", "Name", ""), ("room", "Room number", "")]
    return document("Sign in", _heading(variant) + _notice(notice) + _form(fields, action=action))


def room_name(variant, *, notice="") -> str:
    fields = [("last_name", "Last name", ""), ("room_number", "Room number", "")]
    return document("Sign in", _heading(variant) + _notice(notice) + _form(fields))


def js_checkbox(variant, *, notice="") -> str:
    body = (
        _heading(variant) + _notice(notice)
        + '<form id="f" method="post" action="accept">'
        '<label><input type="checkbox" id="agree"> I agree to the terms of use</label>'
        '<button type="button" id="go" disabled>Connect</button></form>'
        "<script>"
        "var cb=document.getElementById('agree'),go=document.getElementById('go'),f=document.getElementById('f');"
        "cb.addEventListener('change',function(){go.disabled=!cb.checked;});"
        "go.addEventListener('click',function(){f.submit();});"
        "</script>"
    )
    return document("Sign in", body)


def fetch_dom(variant, *, notice="") -> str:
    body = (
        _heading(variant) + _notice(notice)
        + '<p id="status">Tap Connect to sign in.</p><button type="button" id="go">Connect</button>'
        "<script>"
        "document.getElementById('go').addEventListener('click',function(){"
        "var s=document.getElementById('status');"
        "fetch('accept',{method:'POST'}).then(function(r){s.textContent=r.ok?'Connected':'Failed ('+r.status+')';})"
        ".catch(function(){s.textContent='Failed';});});"
        "</script>"
    )
    return document("Sign in", body)


def _click_then_done(variant, control: str, notice: str) -> str:
    """anchor-hash and div-role-button: a click listener fetches `accept`, then sets location to `done`."""
    body = (
        _heading(variant) + _notice(notice) + control
        + "<script>"
        "document.getElementById('go').addEventListener('click',function(e){e.preventDefault();"
        "fetch('accept',{method:'POST'}).then(function(){location='done';});});"
        "</script>"
    )
    return document("Sign in", body)


def anchor_hash(variant, *, notice="") -> str:
    return _click_then_done(variant, '<p><a href="#" id="go" class="btn">Connect</a></p>', notice)


def div_role_button(variant, *, notice="") -> str:
    return _click_then_done(variant, '<div role="button" tabindex="0" id="go" class="btn">Connect</div>', notice)


def target_blank_post(variant, *, notice="") -> str:
    fields = [("name", "Name", ""), ("room", "Room number", "")]
    return document("Sign in", _heading(variant) + _notice(notice) + _form(fields, attrs=' target="_blank"'))


def window_open(variant, *, notice="") -> str:
    body = (_heading(variant) + _notice(notice)
            + '<button type="button" id="go" onclick="window.open(\'accept-get\')">Connect</button>')
    return document("Sign in", body)


def terms_link(variant, *, notice="") -> str:
    body = (
        _heading(variant) + _notice(notice)
        + '<p><a href="terms">Read the terms of use</a></p>'
        '<form method="post" action="accept">'
        '<label><input type="checkbox" name="agree"> I accept the terms of use</label>'
        '<button type="submit">Connect</button></form>'
    )
    return document("Sign in", body)


def terms(variant) -> str:
    """Opened from the sign-in page; also carries the links Task 15's checks tap."""
    body = (
        "<h1>Terms of use</h1><p>Be kind to the network.</p>"
        '<p><a href="#" onclick="history.back();return false">Back</a></p>'
        '<p><a href="/raw?status=200&amp;body=PK&amp;ctype=application/zip">Download the terms (zip)</a></p>'
        '<p><a href="rmweb:settings">Settings</a></p>'
        '<p><a href="file:///etc/passwd">Local file</a></p>'
    )
    return document("Terms of use", body)


def get_token(variant, *, token, notice="") -> str:
    fields = [("room", "Room", token), ("name", "Name", "Smith")]
    return document("Sign in", _heading(variant) + _notice(notice) + _form(fields, method="get"))


def long_select(variant, *, notice="") -> str:
    options = "".join(f"<option>Country {i:03d}</option>" for i in range(1, 201))
    body = (
        _heading(variant) + _notice(notice)
        + '<p class="note">Documented failure: a 200-option list.</p>'
        f'<form method="post" action="accept"><label>Country<br><select name="country">{options}</select></label>'
        '<button type="submit">Connect</button></form>'
    )
    return document("Sign in", body)


def non_ascii(variant, *, notice="") -> str:
    fields = [("surname", "Surname (type: Müller)", "")]
    body = (_heading(variant) + _notice(notice)
            + '<p class="note">Documented failure: a non-ASCII surname.</p>' + _form(fields))
    return document("Sign in", body)


def tls_two_hosts(variant, *, host_b: str, notice="") -> str:
    """Host A only links to host B; the sign-in form is on B."""
    body = (_heading(variant) + _notice(notice)
            + f'<p>Step 1 of 2 is done.</p><p><a class="btn" href="{escape(host_b)}">Continue on the second host</a></p>')
    return document("Sign in", body)


def tls_two_hosts_b(variant, *, notice="") -> str:
    fields = [("name", "Name", ""), ("room", "Room number", "")]
    return document("Sign in", _heading(variant) + "<p>Step 2 of 2.</p>" + _notice(notice) + _form(fields))


def status_511(variant) -> str:
    body = (f'<h1>Network Authentication Required</h1><p>You need to sign in to use this network.</p>'
            f'<p><a href="/portal/{variant}/">Sign in</a></p>')
    return document("Network Authentication Required", body)


def status_403(variant) -> str:
    """The 403 body is served from /generate_204, so the form action is an absolute path."""
    return plain_form(variant, action=f"/portal/{variant}/accept")


def connected() -> str:
    return document("Connected", "<h1>Connected</h1><p>You are online.</p>")


def connecting() -> str:
    return document("Connecting", "<h1>Connecting…</h1><p>Please wait.</p>")


def waiting() -> str:
    head = '<meta http-equiv="refresh" content="3">'
    return document("Connecting", "<h1>Connecting…</h1><p>This page refreshes every 3 seconds.</p>", head)


def login(variant: str, *, token: str = "", host_b: str = "", notice: str = "") -> str:
    """The sign-in page at /portal/<variant>/."""
    builders = {
        "room-name": room_name, "js-checkbox": js_checkbox, "fetch-dom": fetch_dom, "anchor-hash": anchor_hash,
        "div-role-button": div_role_button, "target-blank-post": target_blank_post, "window-open": window_open,
        "terms-link": terms_link, "long-select": long_select, "non-ascii": non_ascii,
    }
    if variant == "get-token":
        return get_token(variant, token=token, notice=notice)
    if variant == "tls-two-hosts":
        return tls_two_hosts(variant, host_b=host_b, notice=notice)
    if variant in builders:
        return builders[variant](variant, notice=notice)
    return plain_form(variant, notice=notice)  # plain-form, delayed-grant, meta-refresh, cross-origin, ...
