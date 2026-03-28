// Custom nice!view battery status widget for Aurora Corne

import display from 'display';
import { battery } from 'battery';

// Function to display battery percentage
function displayBatteryStatus() {
    const leftBattery = battery.left.percentage;
    const rightBattery = battery.right.percentage;

    display.clear();
    display.drawText(`Left Battery: ${leftBattery}%`, 0, 0);
    display.drawText(`Right Battery: ${rightBattery}%`, 0, 20);
}

// Call the function to display the battery status
setInterval(displayBatteryStatus, 60000); // Update every minute
