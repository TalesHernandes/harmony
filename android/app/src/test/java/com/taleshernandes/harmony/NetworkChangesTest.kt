package com.taleshernandes.harmony

import com.taleshernandes.harmony.net.NetworkChanges
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class NetworkChangesTest {
    @Test fun replacementDoesNotRequireAnEarlierLoss() {
        val changes = NetworkChanges<String>()
        assertFalse(changes.available("wifi"))
        assertTrue(changes.available("cell"))
        assertFalse(changes.lost("wifi"))
        assertFalse(changes.available("cell"))
    }

    @Test fun theSameWifiComingBackIsRecovery() {
        val changes = NetworkChanges<String>()
        changes.available("wifi")
        assertTrue(changes.lost("wifi"))
        assertTrue(changes.available("wifi"))
    }

    @Test fun validationDoesNotRepeatTheNetworkRestart() {
        val changes = NetworkChanges<String>()
        changes.available("wifi")
        assertFalse(changes.capabilities("wifi", setOf(1), false))
        assertFalse(changes.capabilities("wifi", setOf(1), true))
        assertTrue(changes.gainedValidation("wifi"))
        assertFalse(changes.gainedValidation("old"))
        assertFalse(changes.capabilities("wifi", setOf(1), true))
        assertFalse(changes.gainedValidation("wifi"))
        assertFalse(changes.capabilities("old", setOf(0), true))
        assertTrue(changes.capabilities("wifi", setOf(0), true))
    }
}
