/*
 * pin_modal.h — modal that asks for the user's PIN before letting a
 * caller-supplied action run.
 *
 * Used to gate: opening Settings, tapping a thermostat preset, nudging
 * the setpoint with +/-, and toggling schedule mode. Each callsite hands
 * the inner "do the thing" function to pin_gate(); the modal pops up
 * only if settings.pin_enabled is on AND a PIN has been set.
 *
 * The PIN is checked against settings.pin_code (plaintext, 4-6 digits).
 * No session caching — every gated action re-prompts. This matches the
 * "Every action" choice in the UX questionnaire.
 *
 * The modal screen-locks like the existing Settings modals (full-screen
 * 70% black backdrop + centred panel), so nothing else can be tapped
 * while it's open. Cancel closes without running the action; 3 wrong
 * tries close it the same way (silent fail — no lockout).
 */
#ifndef FREETOON_PIN_MODAL_H
#define FREETOON_PIN_MODAL_H

typedef void (*pin_action_cb)(void * ctx);

/* Run `action(ctx)` directly if PIN gating is off / no PIN set; otherwise
 * pop the PIN modal and run it only on correct entry. `ctx` may be NULL.
 * Action is always called on the LVGL thread. */
void pin_gate(pin_action_cb action, void * ctx);

/* True if the gate would prompt — exposed so callsites can suppress UI
 * affordances that only make sense once unlocked (e.g. animated press
 * feedback should not flash before the modal even appears). Most
 * callers can ignore this and just call pin_gate(). */
int  pin_is_armed(void);

#endif
