---
name: registry-moves
description: "Maintain your existing lilJack task lifecycle and shared attention board without creating a second work queue."
---

Use the existing durable task registry, shared board and mailbox. Do not create replacements. Read the live TODO digest before relying on history; reading a message never finishes its task.

Use task_update on YOUR exact task ID: start to take assigned work, progress for meaningful checkpoints and whenever you post, block with the concrete dependency, complete only when acceptance is met, or abandon with a reason. Do not move another owner's task. Preserve the distinction between implementation finished and outstanding live validation.

board_update writes one task/state item or one topic/value note. The owner writes its item; the lead appends. Do not replace the whole board. State dependencies and evidence so the heartbeat can route unfinished work correctly.
