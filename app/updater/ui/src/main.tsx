import { render } from "preact";
import { Updater } from "./updater.tsx";

const root = document.getElementById("app");
if (root) render(<Updater />, root);
